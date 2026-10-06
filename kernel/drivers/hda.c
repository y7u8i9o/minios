/* Driver for Intel High Definition Audio controllers, following the
 * High Definition Audio Specification, revision 1.0a.
 *
 * The controller talks to its codecs through two DMA rings: the driver
 * places verbs in the CORB and the controller returns the codec responses
 * in the RIRB. At probe the driver walks the widget graph of the first
 * audio function group, chooses an output pin, finds a path from a DAC to
 * that pin and unmutes and enables every widget on it.
 *
 * Playback uses the first output stream descriptor. The audio buffer is
 * one physically contiguous 64 KiB block, divided into as many periods
 * of the configured size as fit (at most 256); each period is one entry
 * of the buffer descriptor list with its interrupt-on-completion bit set.
 * The controller plays this hardware ring cyclically, so periods are
 * filled strictly in ring order, and a period is zeroed after it has been
 * played so that an underrun plays silence.
 *
 * The PCM parameters only limit how many periods may be queued at once.
 * The hardware ring is deliberately much longer: QEMU's codec reads up to
 * 8 KiB ahead of its audio backend in one step, and with a ring of only a
 * few periods the controller could wrap around between two position
 * checks, which the driver cannot detect from LPIB.
 *
 * The PCM semantics match virtio-snd (docs/design/audio.md), which lets
 * audiod use either device unchanged. Capture is not implemented. */
#define KLOG_SUBSYS "hda"
#include <drivers/hda.h>
#include <drivers/pci.h>
#include <drivers/timer.h>
#include <drivers/devinfo.h>
#include <audio/pcm.h>
#include <arch/barrier.h>
#include <arch/irq.h>
#include <ipc/signal.h>
#include <minios/abi.h>
#include <mm/memlayout.h>
#include <mm/pmm.h>
#include <mm/slab.h>
#include <mm/vma.h>
#include <mm/vmm.h>
#include <sched/proc.h>
#include <sched/thread.h>
#include <sched/wait.h>
#include <sync/mutex.h>
#include <sync/spinlock.h>
#include <lib/cmdline.h>
#include <lib/printf.h>
#include <lib/string.h>
#include <klog.h>
#include <errno.h>

/* Controller registers (section 3.3 of the specification). */
#define REG_GCAP      0x00
#define REG_VMIN      0x02
#define REG_VMAJ      0x03
#define REG_GCTL      0x08
#define REG_STATESTS  0x0e
#define REG_INTCTL    0x20
#define REG_INTSTS    0x24
#define REG_CORBLBASE 0x40
#define REG_CORBUBASE 0x44
#define REG_CORBWP    0x48
#define REG_CORBRP    0x4a
#define REG_CORBCTL   0x4c
#define REG_CORBSIZE  0x4e
#define REG_RIRBLBASE 0x50
#define REG_RIRBUBASE 0x54
#define REG_RIRBWP    0x58
#define REG_RINTCNT   0x5a
#define REG_RIRBCTL   0x5c
#define REG_RIRBSTS   0x5d
#define REG_RIRBSIZE  0x5e
#define REG_SD_BASE   0x80
#define SD_STRIDE     0x20

/* Stream descriptor registers, relative to the descriptor. */
#define SD_CTL   0x00
#define SD_STS   0x03
#define SD_LPIB  0x04
#define SD_CBL   0x08
#define SD_LVI   0x0c
#define SD_FMT   0x12
#define SD_BDPL  0x18
#define SD_BDPU  0x1c

#define GCTL_CRST     (1u << 0)
#define INTCTL_GIE    (1u << 31)
#define CORBRP_RST    (1u << 15)
#define RIRBWP_RST    (1u << 15)
#define DMA_RUN       (1u << 1)
#define RIRBCTL_RINTCTL (1u << 0)
#define SD_CTL_SRST   (1u << 0)
#define SD_CTL_RUN    (1u << 1)
#define SD_CTL_IOCE   (1u << 2)
#define SD_STS_BCIS   (1u << 2)
#define SD_STS_ALL    0x1c

/* Codec verbs (section 7.3). */
#define VERB_GET_PARAMETER      0xf00
#define VERB_GET_CONN_LIST      0xf02
#define VERB_GET_CONFIG_DEFAULT 0xf1c
#define VERB_SET_CONN_SELECT    0x701
#define VERB_SET_POWER_STATE    0x705
#define VERB_SET_STREAM_CHANNEL 0x706
#define VERB_SET_PIN_CONTROL    0x707
#define VERB_SET_EAPD_BTL       0x70c
#define VERB4_SET_FORMAT        0x2
#define VERB4_SET_AMP           0x3

#define PARAM_VENDOR_ID      0x00
#define PARAM_NODE_COUNT     0x04
#define PARAM_FUNCTION_TYPE  0x05
#define PARAM_WIDGET_CAPS    0x09
#define PARAM_PIN_CAPS       0x0c
#define PARAM_IN_AMP_CAPS    0x0d
#define PARAM_CONN_LIST_LEN  0x0e
#define PARAM_OUT_AMP_CAPS   0x12

#define WIDGET_OUTPUT   0
#define WIDGET_INPUT    1
#define WIDGET_MIXER    2
#define WIDGET_SELECTOR 3
#define WIDGET_PIN      4

#define WCAP_IN_AMP   (1u << 1)
#define WCAP_OUT_AMP  (1u << 2)
#define WCAP_AMP_OVR  (1u << 3)
#define WCAP_CONN_LIST (1u << 8)
#define PINCAP_OUT    (1u << 4)
#define PINCAP_HP     (1u << 3)
#define PINCAP_EAPD   (1u << 16)

#define MAX_WIDGETS  64
#define MAX_CONNS    16
#define MAX_PATH     6
#define STREAM_TAG   1
/* 48 kHz, 16 bits per sample, two channels (section 3.7.1). */
#define STREAM_FORMAT 0x0011

#define MIN_PERIOD_FRAMES 120
#define MAX_PERIOD_FRAMES 2048
#define MAX_PERIODS 8
#define FRAME_BYTES 4
#define BUFFER_ORDER 4             /* 64 KiB, at least MAX_PERIODS * MAX_PERIOD_FRAMES * FRAME_BYTES */
#define BUFFER_BYTES (PAGE_SIZE << BUFFER_ORDER)
#define MAX_HW_PERIODS 256         /* entries of the buffer descriptor list */
#define CODEC_BUFFER_BYTES 8192    /* samples QEMU's codec fetches ahead of playback */
#define COMMAND_TIMEOUT_MS 50

struct widget {
    uint8_t nid;
    uint8_t type;
    uint32_t caps;
    uint32_t pin_caps;
    uint32_t config;
    uint32_t out_amp, in_amp;
    uint8_t nconns;
    uint8_t conns[MAX_CONNS];
};

/* One buffer descriptor list entry (section 3.6.3). */
struct bdl_entry {
    uint64_t addr;
    uint32_t length;
    uint32_t flags;             /* bit 0: interrupt on completion */
};

/* The controller and its playback stream.
 *
 * cmd_lock serializes codec commands. control_lock serializes the
 * configuration requests of the PCM interface (parameters, prepare,
 * start, stop, close). lock protects the ring state below it, which the
 * interrupt handler updates; the waiters of the write and drain paths
 * sleep on waitq with that lock. */
struct hda {
    struct pci_dev *pci;
    volatile uint8_t *regs;
    volatile uint8_t *sd;
    unsigned sd_index;
    struct mutex cmd_lock;
    uint32_t *corb;
    uint64_t *rirb;
    uintptr_t corb_phys, rirb_phys;
    unsigned corb_entries, rirb_entries;
    uint16_t rirb_rp;

    unsigned codec;
    uint32_t vendor;
    unsigned afg;
    struct widget widgets[MAX_WIDGETS];
    unsigned nwidgets;
    uint8_t path[MAX_PATH];         /* pin first, DAC last */
    unsigned path_len;
    const char *pin_kind;

    struct pcm_device pcm;
    struct mutex control_lock;
    int irq;
    const char *irq_kind;

    struct bdl_entry *bdl;
    uintptr_t bdl_phys;
    uint8_t *buffer;
    uintptr_t buffer_phys;

    struct spinlock lock;
    struct waitq waitq;
    struct audio_params params;
    uint32_t period_bytes;
    uint32_t state;                 /* AUDIO_STATE_* */
    unsigned hw_periods;            /* length of the hardware ring */
    bool has_data[MAX_HW_PERIODS];  /* written and not yet played */
    unsigned queued;                /* periods with has_data set, at most params.periods */
    uint64_t completed;             /* periods fetched since the stream started */
    unsigned write;                 /* next period a writer fills */
    unsigned hw;                    /* period the controller is playing */
    uint64_t played_frames;
    uint32_t xruns;
    int last_error;
    bool draining;
};

static struct hda *controller;      /* written once by hda_init before init starts */

static inline uint8_t rd8(struct hda *h, unsigned off) { return *(volatile uint8_t *)(h->regs + off); }
static inline uint16_t rd16(struct hda *h, unsigned off) { return *(volatile uint16_t *)(h->regs + off); }
static inline uint32_t rd32(struct hda *h, unsigned off) { return *(volatile uint32_t *)(h->regs + off); }
static inline void wr8(struct hda *h, unsigned off, uint8_t v) { *(volatile uint8_t *)(h->regs + off) = v; }
static inline void wr16(struct hda *h, unsigned off, uint16_t v) { *(volatile uint16_t *)(h->regs + off) = v; }
static inline void wr32(struct hda *h, unsigned off, uint32_t v) { *(volatile uint32_t *)(h->regs + off) = v; }

static inline uint8_t sd_rd8(struct hda *h, unsigned off) { return *(volatile uint8_t *)(h->sd + off); }
static inline uint32_t sd_rd32(struct hda *h, unsigned off) { return *(volatile uint32_t *)(h->sd + off); }
static inline void sd_wr8(struct hda *h, unsigned off, uint8_t v) { *(volatile uint8_t *)(h->sd + off) = v; }
static inline void sd_wr16(struct hda *h, unsigned off, uint16_t v) { *(volatile uint16_t *)(h->sd + off) = v; }
static inline void sd_wr32(struct hda *h, unsigned off, uint32_t v) { *(volatile uint32_t *)(h->sd + off) = v; }

/* Wait up to timeout_ms for (read() & mask) == value. */
static bool wait_bits8(volatile uint8_t *reg, uint8_t mask, uint8_t value, unsigned timeout_ms)
{
    uint64_t end = timer_ms() + timeout_ms;
    while ((*reg & mask) != value) {
        if (timer_ms() >= end)
            return false;
        sleep_ms(1);
    }
    return true;
}

/* ---- codec commands ---- */

/* Send one verb to the codec and return its response. The controller
 * writes the response into the RIRB; the driver polls the write pointer
 * instead of taking an interrupt, since commands are only sent during
 * probe and configuration. */
static int command(struct hda *h, unsigned nid, uint32_t verb, uint32_t *response)
{
    uint32_t word = (uint32_t)h->codec << 28 | (uint32_t)nid << 20 | (verb & 0xfffff);
    mutex_lock(&h->cmd_lock);
    unsigned wp = ((rd16(h, REG_CORBWP) & 0xff) + 1) % h->corb_entries;
    h->corb[wp] = word;
    wmb();
    wr16(h, REG_CORBWP, (uint16_t)wp);
    uint64_t end = timer_ms() + COMMAND_TIMEOUT_MS;
    int r = -ETIMEDOUT;
    while (timer_ms() < end) {
        uint16_t rirb_wp = rd16(h, REG_RIRBWP) & 0xff;
        while (h->rirb_rp != rirb_wp) {
            h->rirb_rp = (h->rirb_rp + 1) % h->rirb_entries;
            rmb();
            uint64_t entry = h->rirb[h->rirb_rp];
            if (entry >> 32 & (1u << 4))
                continue;           /* unsolicited response */
            if (response)
                *response = (uint32_t)entry;
            r = 0;
        }
        if (r == 0)
            break;
        cpu_relax();
    }
    wr8(h, REG_RIRBSTS, 0x05);
    mutex_unlock(&h->cmd_lock);
    if (r < 0)
        klog_warn("codec %u node %u: no response to verb %05x", h->codec, nid, verb);
    return r;
}

/* A verb with a 12-bit identifier and an 8-bit payload. */
static int verb12(struct hda *h, unsigned nid, unsigned verb, unsigned payload, uint32_t *response)
{
    return command(h, nid, (uint32_t)verb << 8 | (payload & 0xff), response);
}

/* A verb with a 4-bit identifier and a 16-bit payload. */
static int verb4(struct hda *h, unsigned nid, unsigned verb, unsigned payload)
{
    return command(h, nid, (uint32_t)verb << 16 | (payload & 0xffff), NULL);
}

static uint32_t parameter(struct hda *h, unsigned nid, unsigned param)
{
    uint32_t v = 0;
    verb12(h, nid, VERB_GET_PARAMETER, param, &v);
    return v;
}

/* ---- the widget graph ---- */

static struct widget *widget(struct hda *h, unsigned nid)
{
    for (unsigned i = 0; i < h->nwidgets; i++)
        if (h->widgets[i].nid == nid)
            return &h->widgets[i];
    return NULL;
}

/* Read the connection list of w. Short form entries carry 8-bit node IDs,
 * four per response; long form entries carry 16-bit IDs, two per
 * response. Ranges are expanded. */
static void read_connections(struct hda *h, struct widget *w)
{
    uint32_t len = parameter(h, w->nid, PARAM_CONN_LIST_LEN);
    bool long_form = len & 0x80;
    unsigned count = len & 0x7f;
    unsigned per = long_form ? 2 : 4, bits = long_form ? 16 : 8;
    unsigned prev = 0;
    w->nconns = 0;
    for (unsigned i = 0; i < count; i += per) {
        uint32_t v;
        if (verb12(h, w->nid, VERB_GET_CONN_LIST, i, &v) < 0)
            return;
        for (unsigned j = 0; j < per && i + j < count; j++) {
            unsigned entry = (v >> (j * bits)) & ((1u << bits) - 1);
            bool range = entry & (1u << (bits - 1));
            unsigned nid = entry & ((1u << (bits - 1)) - 1);
            unsigned first = range && prev ? prev + 1 : nid;
            for (unsigned n = first; n <= nid && w->nconns < MAX_CONNS; n++)
                w->conns[w->nconns++] = (uint8_t)n;
            prev = nid;
        }
    }
}

/* Find the first audio function group of the codec and read its widgets. */
static int read_widgets(struct hda *h)
{
    h->vendor = parameter(h, 0, PARAM_VENDOR_ID);
    uint32_t nodes = parameter(h, 0, PARAM_NODE_COUNT);
    unsigned start = nodes >> 16 & 0xff, count = nodes & 0xff;
    h->afg = 0;
    for (unsigned nid = start; nid < start + count; nid++)
        if ((parameter(h, nid, PARAM_FUNCTION_TYPE) & 0xff) == 1) {
            h->afg = nid;
            break;
        }
    if (!h->afg)
        return -ENODEV;
    verb12(h, h->afg, VERB_SET_POWER_STATE, 0, NULL);
    nodes = parameter(h, h->afg, PARAM_NODE_COUNT);
    start = nodes >> 16 & 0xff;
    count = nodes & 0xff;
    h->nwidgets = 0;
    for (unsigned nid = start; nid < start + count && h->nwidgets < MAX_WIDGETS; nid++) {
        struct widget *w = &h->widgets[h->nwidgets++];
        memset(w, 0, sizeof *w);
        w->nid = (uint8_t)nid;
        w->caps = parameter(h, nid, PARAM_WIDGET_CAPS);
        w->type = w->caps >> 20 & 0xf;
        /* Without the amplifier override bit, a widget uses the default
         * amplifier parameters of the function group. */
        unsigned amp_node = (w->caps & WCAP_AMP_OVR) ? nid : h->afg;
        if (w->caps & WCAP_OUT_AMP)
            w->out_amp = parameter(h, amp_node, PARAM_OUT_AMP_CAPS);
        if (w->caps & WCAP_IN_AMP)
            w->in_amp = parameter(h, amp_node, PARAM_IN_AMP_CAPS);
        if (w->type == WIDGET_PIN) {
            w->pin_caps = parameter(h, nid, PARAM_PIN_CAPS);
            verb12(h, nid, VERB_GET_CONFIG_DEFAULT, 0, &w->config);
        }
        if (w->caps & WCAP_CONN_LIST)
            read_connections(h, w);
    }
    return 0;
}

/* Depth-first search from node nid towards a DAC. On success the path
 * array contains the nodes from the pin down to the DAC. */
static bool find_dac(struct hda *h, unsigned nid, unsigned depth)
{
    struct widget *w = widget(h, nid);
    if (!w || depth >= MAX_PATH)
        return false;
    for (unsigned i = 0; i < depth; i++)
        if (h->path[i] == nid)
            return false;
    h->path[depth] = (uint8_t)nid;
    if (w->type == WIDGET_OUTPUT) {
        h->path_len = depth + 1;
        return true;
    }
    if (depth > 0 && w->type != WIDGET_MIXER && w->type != WIDGET_SELECTOR)
        return false;
    for (unsigned i = 0; i < w->nconns; i++)
        if (find_dac(h, w->conns[i], depth + 1))
            return true;
    return false;
}

static const char *pin_device_name(unsigned device)
{
    switch (device) {
    case 0x0: return "line out";
    case 0x1: return "speaker";
    case 0x2: return "headphones";
    case 0x5: return "S/PDIF out";
    default:  return "output";
    }
}

/* Choose an output pin with a path to a DAC. Line out comes first, then
 * speakers, headphones and any other output-capable pin, skipping pins
 * whose configuration says nothing is connected. */
static int choose_path(struct hda *h)
{
    static const unsigned preference[] = {0x0, 0x1, 0x2, 0xff};
    for (unsigned p = 0; p < ARRAY_SIZE(preference); p++)
        for (unsigned i = 0; i < h->nwidgets; i++) {
            struct widget *w = &h->widgets[i];
            if (w->type != WIDGET_PIN || !(w->pin_caps & PINCAP_OUT))
                continue;
            unsigned connectivity = w->config >> 30, device = w->config >> 20 & 0xf;
            if (connectivity == 1)
                continue;
            if (preference[p] != 0xff && device != preference[p])
                continue;
            if (find_dac(h, w->nid, 0)) {
                h->pin_kind = pin_device_name(device);
                return 0;
            }
        }
    return -ENODEV;
}

/* The amplifier gain step that corresponds to 0 dB. */
static unsigned amp_0db(uint32_t caps)
{
    unsigned offset = caps & 0x7f, steps = caps >> 8 & 0x7f;
    return offset <= steps ? offset : steps;
}

/* Power up every widget on the path, select the path at each selector or
 * mixer input, set the amplifiers to 0 dB unmuted, enable the pin output
 * and connect the DAC to the playback stream. */
static void configure_path(struct hda *h)
{
    for (unsigned i = 0; i < h->path_len; i++) {
        struct widget *w = widget(h, h->path[i]);
        verb12(h, w->nid, VERB_SET_POWER_STATE, 0, NULL);
        if (i + 1 < h->path_len) {
            unsigned next = h->path[i + 1], index = 0;
            while (index < w->nconns && w->conns[index] != next)
                index++;
            if (w->nconns > 1 && w->type != WIDGET_MIXER)
                verb12(h, w->nid, VERB_SET_CONN_SELECT, index, NULL);
            if (w->caps & WCAP_IN_AMP)
                verb4(h, w->nid, VERB4_SET_AMP, 0x7000 | index << 8 | amp_0db(w->in_amp));
        }
        if (w->caps & WCAP_OUT_AMP)
            verb4(h, w->nid, VERB4_SET_AMP, 0xb000 | amp_0db(w->out_amp));
        if (w->type == WIDGET_PIN) {
            verb12(h, w->nid, VERB_SET_PIN_CONTROL, 0x40 | ((w->pin_caps & PINCAP_HP) ? 0x80 : 0), NULL);
            if (w->pin_caps & PINCAP_EAPD)
                verb12(h, w->nid, VERB_SET_EAPD_BTL, 0x02, NULL);
        }
        if (w->type == WIDGET_OUTPUT) {
            verb4(h, w->nid, VERB4_SET_FORMAT, STREAM_FORMAT);
            verb12(h, w->nid, VERB_SET_STREAM_CHANNEL, STREAM_TAG << 4, NULL);
        }
    }
}

/* ---- controller setup ---- */

static int reset_controller(struct hda *h)
{
    wr32(h, REG_GCTL, rd32(h, REG_GCTL) & ~GCTL_CRST);
    if (!wait_bits8(h->regs + REG_GCTL, GCTL_CRST, 0, 100))
        return -ETIMEDOUT;
    sleep_ms(1);
    wr32(h, REG_GCTL, rd32(h, REG_GCTL) | GCTL_CRST);
    if (!wait_bits8(h->regs + REG_GCTL, GCTL_CRST, GCTL_CRST, 100))
        return -ETIMEDOUT;
    /* Codecs signal their presence within 521 us of the end of reset. */
    sleep_ms(2);
    return 0;
}

/* The largest ring size the controller supports: 256, 16 or 2 entries.
 * Returns the size code for the SIZE register and stores the count. */
static uint8_t ring_size(uint8_t reg, unsigned *entries)
{
    if (reg & 0x40) {
        *entries = 256;
        return 2;
    }
    if (reg & 0x20) {
        *entries = 16;
        return 1;
    }
    *entries = 2;
    return 0;
}

static int setup_rings(struct hda *h)
{
    h->corb = pmm_alloc_dma_page(&h->corb_phys);
    h->rirb = pmm_alloc_dma_page(&h->rirb_phys);
    if (!h->corb || !h->rirb)
        return -ENOMEM;

    wr8(h, REG_CORBCTL, 0);
    wait_bits8(h->regs + REG_CORBCTL, DMA_RUN, 0, 10);
    wr8(h, REG_CORBSIZE, ring_size(rd8(h, REG_CORBSIZE), &h->corb_entries));
    wr32(h, REG_CORBLBASE, (uint32_t)h->corb_phys);
    wr32(h, REG_CORBUBASE, (uint32_t)((uint64_t)h->corb_phys >> 32));
    wr16(h, REG_CORBWP, 0);
    /* Reset the read pointer. Some controllers do not report the reset
     * bit back, so the handshake is bounded and not required. */
    wr16(h, REG_CORBRP, CORBRP_RST);
    uint64_t end = timer_ms() + 10;
    while (!(rd16(h, REG_CORBRP) & CORBRP_RST) && timer_ms() < end)
        cpu_relax();
    wr16(h, REG_CORBRP, 0);
    end = timer_ms() + 10;
    while ((rd16(h, REG_CORBRP) & CORBRP_RST) && timer_ms() < end)
        cpu_relax();
    wr8(h, REG_CORBCTL, DMA_RUN);

    wr8(h, REG_RIRBCTL, 0);
    wait_bits8(h->regs + REG_RIRBCTL, DMA_RUN, 0, 10);
    wr8(h, REG_RIRBSIZE, ring_size(rd8(h, REG_RIRBSIZE), &h->rirb_entries));
    wr32(h, REG_RIRBLBASE, (uint32_t)h->rirb_phys);
    wr32(h, REG_RIRBUBASE, (uint32_t)((uint64_t)h->rirb_phys >> 32));
    wr16(h, REG_RIRBWP, RIRBWP_RST);
    wr16(h, REG_RINTCNT, 1);
    h->rirb_rp = 0;
    /* With RINTCTL set, the controller flags every RINTCNT responses in
     * RIRBSTS; command() acknowledges the flag after each response.
     * QEMU stops reading the CORB while the count is reached and not
     * acknowledged. No interrupt results, since INTCTL.CIE stays clear. */
    wr8(h, REG_RIRBCTL, DMA_RUN | RIRBCTL_RINTCTL);
    return 0;
}

/* ---- the playback stream ---- */

/* Stop the DMA engine of the stream and acknowledge its status bits. */
static void stream_halt(struct hda *h)
{
    sd_wr8(h, SD_CTL, sd_rd8(h, SD_CTL) & ~(SD_CTL_RUN | SD_CTL_IOCE));
    wait_bits8(h->sd + SD_CTL, SD_CTL_RUN, 0, 20);
    sd_wr8(h, SD_STS, SD_STS_ALL);
}

static int stream_reset(struct hda *h)
{
    stream_halt(h);
    sd_wr8(h, SD_CTL, SD_CTL_SRST);
    if (!wait_bits8(h->sd + SD_CTL, SD_CTL_SRST, SD_CTL_SRST, 20))
        return -ETIMEDOUT;
    sd_wr8(h, SD_CTL, 0);
    if (!wait_bits8(h->sd + SD_CTL, SD_CTL_SRST, 0, 20))
        return -ETIMEDOUT;
    return 0;
}

/* Account for the periods the controller finished since the last call.
 * The caller has acquired h->lock. */
static void advance_locked(struct hda *h)
{
    if (h->state != AUDIO_STATE_RUNNING || !h->period_bytes)
        return;
    unsigned n = h->hw_periods;
    unsigned cur = sd_rd32(h, SD_LPIB) / h->period_bytes;
    if (cur >= n)
        cur = n - 1;
    bool progressed = false;
    while (h->hw != cur) {
        unsigned done = h->hw;
        if (h->has_data[done]) {
            h->has_data[done] = false;
            h->queued--;
            h->played_frames += h->params.period_frames;
        }
        /* The controller is now reading a later period, so the finished
         * one can be cleared: if the writer falls behind, the controller
         * plays silence from it instead of stale samples. */
        memset(h->buffer + done * h->period_bytes, 0, h->period_bytes);
        h->hw = (done + 1) % n;
        h->completed++;
        if (h->queued == 0 && !h->draining)
            h->xruns++;
        progressed = true;
    }
    if (progressed) {
        waitq_wake_all(&h->waitq);
        poll_source_notify(&h->pcm.poll);
    }
}

/* The period a writer may fill next, or -1 when params.periods periods
 * are already queued. When nothing is queued on a running stream, the
 * controller is playing silence, and the writer continues with the period
 * after the one being played. The caller has acquired h->lock. */
static int writable_period_locked(struct hda *h)
{
    if (h->queued >= h->params.periods)
        return -1;
    if (h->state == AUDIO_STATE_RUNNING && h->queued == 0)
        h->write = (h->hw + 1) % h->hw_periods;
    return (int)h->write;
}

static void hda_irq(struct trapframe *tf, void *arg)
{
    struct hda *h = arg;
    uint32_t status = rd32(h, REG_INTSTS);
    if (!(status & (1u << h->sd_index)))
        return;
    sd_wr8(h, SD_STS, SD_STS_ALL);
    spin_lock(&h->lock);
    advance_locked(h);
    spin_unlock(&h->lock);
}

/* Without an interrupt, a kernel thread checks the stream position every
 * 2 ms, which is well below the shortest period of 2.5 ms. */
static void poll_thread(void *arg)
{
    struct hda *h = arg;
    for (;;) {
        sleep_ms(2);
        spin_lock(&h->lock);
        advance_locked(h);
        spin_unlock(&h->lock);
    }
}

static bool params_valid(const struct audio_params *p)
{
    return p->format == AUDIO_FORMAT_S16_LE && p->rate == 48000 && p->channels == 2 &&
           p->period_frames >= MIN_PERIOD_FRAMES && p->period_frames <= MAX_PERIOD_FRAMES &&
           p->periods >= 2 && p->periods <= MAX_PERIODS;
}

static int stream_prepare(struct hda *h)
{
    spin_lock(&h->lock);
    bool ready = h->state == AUDIO_STATE_OPEN;
    spin_unlock(&h->lock);
    if (!ready)
        return -EINVAL;
    int r = stream_reset(h);
    if (r < 0)
        return r;
    uint32_t period_bytes = h->params.period_frames * FRAME_BYTES;
    unsigned n = MIN(BUFFER_BYTES / period_bytes, MAX_HW_PERIODS);
    memset(h->buffer, 0, BUFFER_BYTES);
    for (unsigned i = 0; i < n; i++) {
        h->bdl[i].addr = h->buffer_phys + i * period_bytes;
        h->bdl[i].length = period_bytes;
        h->bdl[i].flags = 1;
    }
    wmb();
    sd_wr32(h, SD_BDPL, (uint32_t)h->bdl_phys);
    sd_wr32(h, SD_BDPU, (uint32_t)((uint64_t)h->bdl_phys >> 32));
    sd_wr32(h, SD_CBL, n * period_bytes);
    sd_wr16(h, SD_LVI, (uint16_t)(n - 1));
    sd_wr16(h, SD_FMT, STREAM_FORMAT);
    /* The stream tag is in bits 23:20 of the control register. */
    sd_wr8(h, SD_CTL + 2, STREAM_TAG << 4);
    spin_lock(&h->lock);
    h->period_bytes = period_bytes;
    h->hw_periods = n;
    memset(h->has_data, 0, sizeof h->has_data);
    h->queued = h->write = h->hw = 0;
    h->draining = false;
    h->last_error = 0;
    h->state = AUDIO_STATE_PREPARED;
    spin_unlock(&h->lock);
    return 0;
}

static int stream_start(struct hda *h)
{
    spin_lock(&h->lock);
    bool ready = h->state == AUDIO_STATE_PREPARED && h->queued >= 2;
    if (ready)
        h->state = AUDIO_STATE_RUNNING;
    spin_unlock(&h->lock);
    if (!ready)
        return -EAGAIN;
    sd_wr8(h, SD_STS, SD_STS_ALL);
    sd_wr8(h, SD_CTL, sd_rd8(h, SD_CTL) | SD_CTL_RUN | (h->irq >= 0 ? SD_CTL_IOCE : 0));
    return 0;
}

/* Let the queued periods play out, then stop the controller.
 *
 * The controller fetches samples before the codec plays them, and
 * stopping the stream discards whatever the codec has buffered. QEMU's
 * codec buffers up to 8 KiB, so after the last queued period has been
 * fetched the stream runs on through silent periods until at least that
 * much more has been fetched. Both waits are bounded, so a controller
 * that stopped delivering positions cannot hang the caller. */
static int stream_stop(struct hda *h)
{
    spin_lock(&h->lock);
    uint32_t state = h->state;
    if (state != AUDIO_STATE_PREPARED && state != AUDIO_STATE_RUNNING && state != AUDIO_STATE_ERROR) {
        spin_unlock(&h->lock);
        return 0;
    }
    h->draining = true;
    int r = 0;
    if (state == AUDIO_STATE_RUNNING) {
        uint64_t ring_ms = (uint64_t)h->params.periods * h->params.period_frames / 48;
        uint64_t deadline = timer_ms() + 2 * ring_ms + 100;
        while (h->queued && timer_ms() < deadline)
            waitq_wait_timeout(&h->waitq, &h->lock, deadline);
        if (h->queued) {
            klog_warn("drain: %u periods not played", h->queued);
            r = -EIO;
        } else {
            uint64_t target = h->completed + DIV_ROUND_UP(CODEC_BUFFER_BYTES, h->period_bytes) + 1;
            deadline = timer_ms() + 2 * CODEC_BUFFER_BYTES / (48 * FRAME_BYTES) + 100;
            while (h->completed < target && timer_ms() < deadline)
                waitq_wait_timeout(&h->waitq, &h->lock, deadline);
        }
    }
    spin_unlock(&h->lock);
    stream_halt(h);
    spin_lock(&h->lock);
    h->state = AUDIO_STATE_OPEN;
    h->queued = 0;
    memset(h->has_data, 0, sizeof h->has_data);
    h->draining = false;
    h->last_error = r;
    h->period_bytes = 0;
    spin_unlock(&h->lock);
    waitq_wake_all(&h->waitq);
    poll_source_notify(&h->pcm.poll);
    return r;
}

/* ---- the PCM interface ---- */

static int hda_open(struct pcm_device *pcm, struct file *f)
{
    struct hda *h = pcm->priv;
    mutex_lock(&h->control_lock);
    spin_lock(&h->lock);
    h->params = (struct audio_params) {
        .format = AUDIO_FORMAT_S16_LE,
        .rate = 48000,
        .channels = 2,
        .period_frames = 480,
        .periods = MAX_PERIODS,
    };
    h->played_frames = 0;
    h->xruns = 0;
    h->last_error = 0;
    h->state = AUDIO_STATE_OPEN;
    spin_unlock(&h->lock);
    mutex_unlock(&h->control_lock);
    return 0;
}

static long hda_write(struct pcm_device *pcm, struct file *f, const char *buf, size_t n)
{
    struct hda *h = pcm->priv;
    spin_lock(&h->lock);
    int slot;
    for (;;) {
        if (h->state != AUDIO_STATE_PREPARED && h->state != AUDIO_STATE_RUNNING) {
            int r = h->state == AUDIO_STATE_ERROR ? -EIO : -EINVAL;
            spin_unlock(&h->lock);
            return r;
        }
        if (n != h->period_bytes) {
            spin_unlock(&h->lock);
            return -EINVAL;
        }
        advance_locked(h);
        slot = writable_period_locked(h);
        if (slot >= 0)
            break;
        if (f->flags & O_NONBLOCK) {
            spin_unlock(&h->lock);
            return -EAGAIN;
        }
        if (signal_should_interrupt()) {
            spin_unlock(&h->lock);
            return -EINTR;
        }
        waitq_wait(&h->waitq, &h->lock);
    }
    /* The controller does not read this period until has_data is set and
     * the ring advances to it, so the copy can run without the lock. The
     * writer is the only owner of the file, since the PCM core makes the
     * device exclusive. */
    h->write = ((unsigned)slot + 1) % h->hw_periods;
    spin_unlock(&h->lock);
    memcpy(h->buffer + (unsigned)slot * n, buf, n);
    wmb();
    spin_lock(&h->lock);
    h->has_data[slot] = true;
    h->queued++;
    spin_unlock(&h->lock);
    return (long)n;
}

static long hda_read(struct pcm_device *pcm, struct file *f, char *buf, size_t n)
{
    return -ENODEV;
}

static long hda_ioctl(struct pcm_device *pcm, struct file *f, unsigned long req, uintptr_t arg)
{
    struct hda *h = pcm->priv;
    struct proc *proc = thread_current()->proc;
    long r = 0;
    mutex_lock(&h->control_lock);
    switch (req) {
    case AUDIO_GET_INFO: {
        if (!vma_range_ok(proc->vm, arg, sizeof(struct audio_info), true)) {
            r = -EFAULT;
            break;
        }
        struct audio_info info = {
            .abi_version = AUDIO_ABI_VERSION,
            .capabilities = AUDIO_CAP_PLAYBACK,
            .formats = AUDIO_FORMAT_S16_LE,
            .rates = AUDIO_RATE_48000,
            .channels_min = 2,
            .channels_max = 2,
            .period_frames_min = MIN_PERIOD_FRAMES,
            .period_frames_max = MAX_PERIOD_FRAMES,
            .periods_min = 2,
            .periods_max = MAX_PERIODS,
        };
        memcpy((void *)arg, &info, sizeof info);
        break;
    }
    case AUDIO_SET_PARAMS: {
        if (!vma_range_ok(proc->vm, arg, sizeof(struct audio_params), false)) {
            r = -EFAULT;
            break;
        }
        struct audio_params params;
        memcpy(&params, (void *)arg, sizeof params);
        if (!params_valid(&params)) {
            r = -EINVAL;
            break;
        }
        spin_lock(&h->lock);
        if (h->state != AUDIO_STATE_OPEN)
            r = -EBUSY;
        else
            h->params = params;
        spin_unlock(&h->lock);
        break;
    }
    case AUDIO_PREPARE:
        r = stream_prepare(h);
        break;
    case AUDIO_START:
        r = stream_start(h);
        break;
    case AUDIO_DROP:
    case AUDIO_DRAIN:
        r = stream_stop(h);
        break;
    case AUDIO_GET_STATUS: {
        if (!vma_range_ok(proc->vm, arg, sizeof(struct audio_status), true)) {
            r = -EFAULT;
            break;
        }
        spin_lock(&h->lock);
        advance_locked(h);
        struct audio_status status = {
            .state = h->state,
            .queued_frames = h->queued * h->params.period_frames,
            .played_frames = h->played_frames,
            .xruns = h->xruns,
            .last_error = h->last_error,
        };
        spin_unlock(&h->lock);
        memcpy((void *)arg, &status, sizeof status);
        break;
    }
    case AUDIO_SET_CAPTURE_PARAMS:
    case AUDIO_CAPTURE_PREPARE:
    case AUDIO_CAPTURE_START:
    case AUDIO_CAPTURE_DROP:
    case AUDIO_GET_CAPTURE_STATUS:
        r = -ENODEV;
        break;
    default:
        r = -ENOTTY;
        break;
    }
    mutex_unlock(&h->control_lock);
    return r;
}

static int hda_poll(struct pcm_device *pcm, struct file *f)
{
    struct hda *h = pcm->priv;
    int r = 0;
    spin_lock(&h->lock);
    if (h->state == AUDIO_STATE_ERROR)
        r |= POLLERR;
    if (h->state == AUDIO_STATE_PREPARED || h->state == AUDIO_STATE_RUNNING) {
        advance_locked(h);
        if (writable_period_locked(h) >= 0)
            r |= POLLOUT;
    }
    spin_unlock(&h->lock);
    return r;
}

static void hda_close(struct pcm_device *pcm, struct file *f)
{
    struct hda *h = pcm->priv;
    mutex_lock(&h->control_lock);
    stream_stop(h);
    spin_lock(&h->lock);
    h->state = AUDIO_STATE_CLOSED;
    spin_unlock(&h->lock);
    mutex_unlock(&h->control_lock);
}

static const char *state_name(uint32_t s)
{
    switch (s) {
    case AUDIO_STATE_CLOSED:   return "closed";
    case AUDIO_STATE_OPEN:     return "open";
    case AUDIO_STATE_PREPARED: return "prepared";
    case AUDIO_STATE_RUNNING:  return "running";
    case AUDIO_STATE_ERROR:    return "error";
    default:                   return "unknown";
    }
}

static void hda_describe(struct pcm_device *pcm, struct devinfo *d)
{
    struct hda *h = pcm->priv;
    devinfo_prop(d, "driver", "hda");
    devinfo_prop(d, "pci_address", "%02x:%02x.%u", h->pci->bus, h->pci->slot, h->pci->func);
    devinfo_prop(d, "codec", "%u, vendor %04x:%04x", h->codec, h->vendor >> 16, h->vendor & 0xffff);
    devinfo_prop(d, "output", "%s, pin %u, DAC %u", h->pin_kind, h->path[0], h->path[h->path_len - 1]);
    devinfo_prop(d, "interrupt", "%s", h->irq_kind);
    devinfo_prop(d, "supported_format", "signed 16 bit little endian, 48000 Hz, 2 channels");
    spin_lock(&h->lock);
    devinfo_prop(d, "playback", "%s, %u periods of %u frames, %lu frames played, %u xruns",
                 state_name(h->state), h->params.periods, h->params.period_frames,
                 (unsigned long)h->played_frames, h->xruns);
    spin_unlock(&h->lock);
    devinfo_prop(d, "capture", "not supported");
}

static const struct pcm_ops hda_pcm_ops = {
    .open = hda_open,
    .read = hda_read,
    .write = hda_write,
    .ioctl = hda_ioctl,
    .poll = hda_poll,
    .close = hda_close,
    .describe = hda_describe,
};

/* Use MSI when the controller offers it, otherwise a polling thread. The
 * kernel option hda=poll forces polling, which lets a test cover it. */
static void setup_interrupt(struct hda *h)
{
    h->irq = -1;
    h->irq_kind = "no";
    char opt[8];
    bool poll = cmdline_lookup("hda", opt, sizeof opt) && strcmp(opt, "poll") == 0;
    int irq = poll ? -1 : irq_alloc();
    if (irq >= 0) {
        irq_register((unsigned)irq, hda_irq, h);
        if (pci_msi_enable(h->pci, (unsigned)irq) == 0) {
            h->irq = irq;
            h->irq_kind = "msi";
            wr32(h, REG_INTCTL, INTCTL_GIE | 1u << h->sd_index);
            return;
        }
    }
    wr32(h, REG_INTCTL, 0);
    if (thread_create("hda", poll_thread, h, 0))
        h->irq_kind = "polled";
}

static bool is_hda(const struct pci_dev *p)
{
    /* Class 04 (multimedia), subclass 03 (HD Audio). */
    return p->class == 0x04 && p->subclass == 0x03 && !p->driver && !p->bar_is_io[0] && p->bar[0];
}

static int probe(struct pci_dev *pci)
{
    struct hda *h = kzalloc(sizeof *h);
    if (!h)
        return -ENOMEM;
    h->pci = pci;
    mutex_init(&h->cmd_lock, "hda_command");
    mutex_init(&h->control_lock, "hda_control");
    spinlock_init(&h->lock, "hda");
    waitq_init(&h->waitq, "hda");
    pci_enable_bus_master(pci);
    h->regs = vmm_map_mmio(pci->bar[0], ALIGN_UP(pci_bar_size(pci, 0), PAGE_SIZE), VM_KERNEL_RW | VM_NOCACHE);
    if (!h->regs)
        return -ENOMEM;
    uint16_t gcap = rd16(h, REG_GCAP);
    unsigned iss = gcap >> 8 & 0xf, oss = gcap >> 12 & 0xf;
    if (!oss) {
        klog_error("%02x:%02x.%u: no output stream", pci->bus, pci->slot, pci->func);
        return -ENODEV;
    }
    int r = reset_controller(h);
    if (r < 0) {
        klog_error("%02x:%02x.%u: controller does not leave reset", pci->bus, pci->slot, pci->func);
        return r;
    }
    wr32(h, REG_INTCTL, 0);
    uint16_t codecs = rd16(h, REG_STATESTS);
    wr16(h, REG_STATESTS, codecs);
    if ((r = setup_rings(h)) < 0)
        return r;
    r = -ENODEV;
    for (unsigned c = 0; c < 15 && r < 0; c++) {
        if (!(codecs & (1u << c)))
            continue;
        h->codec = c;
        if (read_widgets(h) == 0)
            r = choose_path(h);
    }
    if (r < 0) {
        klog_error("%02x:%02x.%u: no codec with an output path", pci->bus, pci->slot, pci->func);
        return r;
    }
    configure_path(h);

    h->bdl = pmm_alloc_dma_page(&h->bdl_phys);
    struct page *pg = pmm_alloc(BUFFER_ORDER);
    if (!h->bdl || !pg)
        return -ENOMEM;
    h->buffer_phys = page_to_phys(pg);
    h->buffer = phys_to_virt(h->buffer_phys);
    h->sd_index = iss;
    h->sd = h->regs + REG_SD_BASE + SD_STRIDE * iss;
    if ((r = stream_reset(h)) < 0)
        return r;
    setup_interrupt(h);
    h->state = AUDIO_STATE_CLOSED;
    pci->driver = "hda";

    char name[16];
    pcm_free_name(name, sizeof name);
    if ((r = pcm_register(&h->pcm, name, &hda_pcm_ops, h)) < 0)
        return r;
    controller = h;
    char path[3 * MAX_PATH + 1] = "", *p = path;
    for (unsigned i = 0; i < h->path_len; i++)
        p += ksnprintf(p, sizeof path - (size_t)(p - path), i ? "-%u" : "%u", h->path[i]);
    klog_info("%s: HDA %u.%u controller %02x:%02x.%u, codec %u (%04x:%04x), %s path %s, %s interrupt",
              name, rd8(h, REG_VMAJ), rd8(h, REG_VMIN), pci->bus, pci->slot, pci->func, h->codec,
              h->vendor >> 16, h->vendor & 0xffff, h->pin_kind, path, h->irq_kind);
    return 0;
}

void hda_init(void)
{
    for (size_t i = 0; i < pci_count(); i++) {
        struct pci_dev *p = pci_device(i);
        if (!is_hda(p))
            continue;
        /* A failed probe leaves its memory allocated: the controller may
         * have been given ring addresses before the failure. */
        probe(p);
        return;
    }
}

bool hda_present(void)
{
    return controller != NULL;
}
