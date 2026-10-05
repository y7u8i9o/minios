/* xHCI host controllers (D2, docs/design/usb.md), written from the eXtensible
 * Host Controller Interface specification, revision 1.2. The driver takes
 * the controller from the firmware, resets it and gives it a device context
 * array, scratchpad buffers, a command ring and one event ring. A thread
 * per controller handles the ports: it resets a port that connects, gives
 * the device an address and passes it to the USB core (usb.c), and it
 * releases the slot of a device that disconnects. R4 adds bulk endpoints
 * for mass storage and the devices behind hubs. The slot context of such a
 * device (route string, root port, transaction translator) and the hub
 * fields of a hub's slot follow XhcInitializeDeviceSlot and
 * XhcConfigHubContext in MdeModulePkg/Bus/Pci/XhciDxe/XhciSched.c of
 * edk2, revision 999fd0f12a27709eee04b93e46bd867e6b0163a5
 * (third_party/edk2/README):
 *
 *   (C) Copyright 2023 Hewlett Packard Enterprise Development LP
 *   Copyright (c) 2011 - 2020, Intel Corporation. All rights reserved.
 *   Copyright (c) Microsoft Corporation.
 *   Copyright (C) 2022 Advanced Micro Devices, Inc. All rights reserved.
 *   Copyright (C) 2025 Qualcomm Technologies, Inc. All rights reserved.
 *   SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * The licence text is third_party/edk2/License.txt. */
#define KLOG_SUBSYS "xhci"
#include "usb.h"
#include <drivers/usb.h>
#include <drivers/pci.h>
#include <drivers/timer.h>
#include <arch/barrier.h>
#include <arch/irq.h>
#include <mm/memlayout.h>
#include <mm/pmm.h>
#include <mm/slab.h>
#include <mm/vmm.h>
#include <sched/thread.h>
#include <sched/wait.h>
#include <sync/spinlock.h>
#include <sync/mutex.h>
#include <drivers/devinfo.h>
#include <lib/printf.h>
#include <lib/string.h>
#include <klog.h>
#include <errno.h>

/* Capability registers. */
#define CAP_CAPLENGTH   0x00
#define CAP_HCIVERSION  0x02
#define CAP_HCSPARAMS1  0x04
#define CAP_HCSPARAMS2  0x08
#define CAP_HCCPARAMS1  0x10
#define CAP_DBOFF       0x14
#define CAP_RTSOFF      0x18

/* Operational registers. */
#define OP_USBCMD       0x00
#define OP_USBSTS       0x04
#define OP_PAGESIZE     0x08
#define OP_CRCR         0x18
#define OP_DCBAAP       0x30
#define OP_CONFIG       0x38
#define OP_PORTSC(n)    (0x400 + 0x10 * ((n) - 1))

#define USBCMD_RS       (1u << 0)
#define USBCMD_HCRST    (1u << 1)
#define USBCMD_INTE     (1u << 2)
#define USBSTS_HCH      (1u << 0)
#define USBSTS_EINT     (1u << 3)
#define USBSTS_CNR      (1u << 11)

/* Port status and control. The change bits are cleared by writing 1. PED
 * is cleared by writing 1 too, which disables the port, so a write never
 * sets it. */
#define PORTSC_CCS      (1u << 0)
#define PORTSC_PED      (1u << 1)
#define PORTSC_PR       (1u << 4)
#define PORTSC_PP       (1u << 9)
#define PORTSC_SPEED(v) (((v) >> 10) & 0xf)
#define PORTSC_CSC      (1u << 17)
#define PORTSC_PRC      (1u << 21)
#define PORTSC_WRC      (1u << 19)
#define PORTSC_WPR      (1u << 31)
#define PORTSC_CHANGES  0x00fe0000u     /* CSC, PEC, WRC, OCC, PRC, PLC and CEC */
#define PORTSC_PRESERVE 0x0e00c200u     /* PP, the indicator and the wake enables */

/* Interrupter 0 in the runtime registers. */
#define RT_IMAN         0x20
#define RT_IMOD         0x24
#define RT_ERSTSZ       0x28
#define RT_ERSTBA       0x30
#define RT_ERDP         0x38
#define IMAN_IP         (1u << 0)
#define IMAN_IE         (1u << 1)
#define ERDP_EHB        (1u << 3)

/* Extended capabilities. */
#define XCAP_LEGACY     1
#define XCAP_PROTOCOL   2
#define LEGACY_BIOS_OWNED (1u << 16)
#define LEGACY_OS_OWNED   (1u << 24)

/* Transfer request blocks. */
#define TRB_NORMAL              1
#define TRB_SETUP               2
#define TRB_DATA                3
#define TRB_STATUS              4
#define TRB_LINK                6
#define TRB_ENABLE_SLOT         9
#define TRB_DISABLE_SLOT        10
#define TRB_ADDRESS_DEVICE      11
#define TRB_CONFIGURE_EP        12
#define TRB_EVALUATE_CONTEXT    13
#define TRB_RESET_EP            14
#define TRB_STOP_EP             15
#define TRB_SET_TR_DEQUEUE      16
#define TRB_TRANSFER_EVENT      32
#define TRB_COMMAND_COMPLETION  33
#define TRB_PORT_STATUS_CHANGE  34

#define TRB_CYCLE       (1u << 0)
#define TRB_TC          (1u << 1)       /* link: toggle the cycle bit */
#define TRB_ISP         (1u << 2)       /* event on a short packet */
#define TRB_IOC         (1u << 5)       /* event on completion */
#define TRB_IDT         (1u << 6)       /* the parameter is the data */
#define TRB_DIR_IN      (1u << 16)
#define TRB_TYPE(t)     ((uint32_t)(t) << 10)
#define TRB_SLOT(s)     ((uint32_t)(s) << 24)
#define TRB_EP(e)       ((uint32_t)(e) << 16)

#define CC_SUCCESS      1
#define CC_STALL        6
#define CC_SHORT_PACKET 13

#define EP_TYPE_BULK_OUT 2
#define EP_TYPE_CONTROL 4
#define EP_TYPE_BULK_IN 6
#define EP_TYPE_INT_IN  7

#define RING_TRBS       (PAGE_SIZE / 16)        /* one page per ring */
#define MAX_SLOTS       32
#define MAX_DCI         32                      /* device context indexes 1 to 31 */
#define MAX_PORTS       256
#define MAX_CONTROLLERS 4
#define CMD_TIMEOUT_MS  1000
#define CTRL_TIMEOUT_MS 2000
#define POLL_MS         10                      /* event polling period without an interrupt */
#define EP_RETRIES      5                       /* resets of a failing interrupt endpoint */
#define BULK_ORDER      4                       /* the bounce block of a bulk endpoint, XHCI_BULK_MAX bytes */

struct trb {
    uint64_t param;
    uint32_t status;
    uint32_t control;
} __packed;

/* A producer ring of one page: a command ring or a transfer ring. The last
 * TRB is a link to the start. */
struct ring {
    struct trb *trb;
    uintptr_t phys;
    unsigned enqueue;
    uint32_t cycle;
};

/* An interrupt IN endpoint, a bulk endpoint, or endpoint 0 (index 1),
 * which uses the ring only. */
struct endpoint {
    struct ring ring;
    usb_report_fn fn;
    void *arg;
    uint8_t *buf;
    uintptr_t buf_phys;
    unsigned len;
    bool active;            /* a transfer is queued and its completion is reported */
    bool halted;            /* the transfer failed, and the thread resets the endpoint */
    unsigned failures;      /* consecutive failures, reset by a successful transfer */
    bool bulk;              /* a bulk endpoint of xhci_bulk_open */
    struct page *bulk_pages;    /* the bounce block of 2^BULK_ORDER pages */
    bool done;              /* the bulk transfer completed */
    uint32_t cc, residual;  /* its completion code and the bytes not transferred */
};

/* A device slot. dev is filled by the controller thread. The rings, the
 * endpoint state, the control completion fields and dev.gone are
 * protected by xhci.lock. ctrl_lock serializes the control transfers of
 * the slot, which the controller thread and the threads of the class
 * drivers issue. */
struct slot {
    struct usb_device dev;
    struct mutex ctrl_lock;
    void *out_ctx;          /* the device context that the controller writes */
    uintptr_t out_phys;
    void *in_ctx;           /* the input context of the commands */
    uintptr_t in_phys;
    uint8_t *ctrl_buf;      /* the data stage of the control transfers */
    uintptr_t ctrl_phys;
    struct endpoint ep[MAX_DCI];
    unsigned max_dci;
    bool ctrl_done;
    uint32_t ctrl_cc;
};

/* One controller.
 *
 * lock protects the command ring and the command completion fields, the
 * event ring dequeue state, the slot table slots[], the rings and endpoint
 * state of every slot, port_pending, ep_work and initial_done. The
 * interrupt handler and the controller thread take it. The report
 * callbacks of the class drivers run under it, which gives the order
 * xhci.lock -> input_dev.lock. waitq.lock is taken under it to wake the
 * thread.
 *
 * port_slot[] is written by the controller thread under usb_topology_lock
 * and read by that thread without it, and by usb_describe under it. */
struct xhci {
    struct pci_dev *pci;
    unsigned index;
    unsigned version;                           /* HCIVERSION */
    unsigned scratch_count;                     /* scratchpad buffers given to the controller */
    volatile uint8_t *cap, *op, *rt, *db;
    unsigned max_slots, max_ports, ctx_size;
    uint8_t port_major[MAX_PORTS + 1];          /* USB major revision of each port */
    uint64_t *dcbaa;
    uintptr_t dcbaa_phys;
    uint64_t *scratch;                          /* the scratchpad buffer array */
    struct ring cmd;
    struct trb *events;
    uintptr_t events_phys;
    uint64_t *erst;
    uintptr_t erst_phys;
    unsigned ev_dequeue;
    uint32_t ev_cycle;
    int irq;                                    /* the interrupt number, -1 when polled */
    const char *irq_kind;

    struct spinlock lock;
    struct mutex cmd_lock;                      /* serializes the commands */
    struct waitq waitq;                         /* the thread, the completion waits and usb_init */
    uintptr_t cmd_trb;                          /* the command being waited for */
    bool cmd_done;
    uint32_t cmd_cc;
    unsigned cmd_slot;
    uint64_t port_pending[MAX_PORTS / 64];      /* ports with a status change */
    bool ep_work;                               /* an endpoint is halted */
    bool hub_work;                              /* a hub reported a port change */
    bool initial_done;                          /* the ports present at boot are enumerated */
    struct slot *slots[MAX_SLOTS + 1];

    struct slot *port_slot[MAX_PORTS + 1];
};

static struct xhci *controllers[MAX_CONTROLLERS];
static unsigned ncontrollers;                   /* written by usb_init before the threads start */

/* The devices published on the ports of every controller and of every hub.
 * The controller threads take it to add a device to port_slot[] or to the
 * ports of a hub after its enumeration and to remove it before its slot is
 * freed. usb_describe takes it while it reads the devices. It is a
 * sleeping lock, taken with no spinlock acquired, before xhci.lock. */
struct mutex usb_topology_lock;

static inline uint32_t rd(volatile uint8_t *base, unsigned off)
{
    return *(volatile uint32_t *)(base + off);
}

static inline void wr(volatile uint8_t *base, unsigned off, uint32_t v)
{
    *(volatile uint32_t *)(base + off) = v;
}

/* A 64 bit register written as two 32 bit halves, the low half first. */
static inline void wr64(volatile uint8_t *base, unsigned off, uint64_t v)
{
    wr(base, off, (uint32_t)v);
    wr(base, off + 4, (uint32_t)(v >> 32));
}

static int ring_init(struct ring *r)
{
    r->trb = pmm_alloc_dma_page(&r->phys);
    if (!r->trb)
        return -ENOMEM;
    r->enqueue = 0;
    r->cycle = 1;
    return 0;
}

/* Append one TRB and return its physical address. The control word
 * contains the cycle bit, and the cycle bit hands the TRB to the
 * controller. The control word is therefore written last. At the link TRB
 * the producer cycle state toggles. */
static uintptr_t ring_push(struct ring *r, uint64_t param, uint32_t status, uint32_t control)
{
    struct trb *t = &r->trb[r->enqueue];
    t->param = param;
    t->status = status;
    __atomic_store_n(&t->control, control | r->cycle, __ATOMIC_RELEASE);
    uintptr_t phys = r->phys + r->enqueue * sizeof *t;
    if (++r->enqueue == RING_TRBS - 1) {
        struct trb *link = &r->trb[RING_TRBS - 1];
        link->param = r->phys;
        link->status = 0;
        __atomic_store_n(&link->control, TRB_TYPE(TRB_LINK) | TRB_TC | r->cycle, __ATOMIC_RELEASE);
        r->cycle ^= 1;
        r->enqueue = 0;
    }
    return phys;
}

/* The physical address of the next TRB, with the producer cycle state in
 * bit 0, as the Set TR Dequeue Pointer command takes it. */
static uint64_t ring_dequeue_pointer(const struct ring *r)
{
    return (r->phys + r->enqueue * sizeof(struct trb)) | r->cycle;
}

static void doorbell(struct xhci *x, unsigned slot, unsigned target)
{
    wmb();
    wr(x->db, slot * 4, target);
}

/* Contexts: the input context starts with the input control context, and
 * the slot context and the endpoint contexts follow, each ctx_size bytes.
 * The device context starts with the slot context. */
static uint32_t *in_ctx(struct xhci *x, struct slot *s, unsigned index)
{
    return (uint32_t *)((uint8_t *)s->in_ctx + index * x->ctx_size);
}

static uint32_t *out_ctx(struct xhci *x, struct slot *s, unsigned index)
{
    return (uint32_t *)((uint8_t *)s->out_ctx + index * x->ctx_size);
}

static void queue_interrupt(struct xhci *x, struct slot *s, unsigned dci)
{
    struct endpoint *ep = &s->ep[dci];
    ring_push(&ep->ring, ep->buf_phys, ep->len, TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP);
    doorbell(x, s->dev.slot, dci);
}

/* A transfer event. Endpoint 0 completes the control transfer that the
 * thread waits for. An interrupt endpoint passes its report on and is
 * queued again. Returns whether a waiter is to be woken. The caller has
 * acquired x->lock. */
static bool transfer_event(struct xhci *x, uint32_t status, uint32_t control)
{
    unsigned slot = control >> 24, dci = (control >> 16) & 0x1f;
    unsigned cc = status >> 24;
    uint32_t residual = status & 0xffffff;
    struct slot *s = slot >= 1 && slot <= MAX_SLOTS ? x->slots[slot] : NULL;
    if (!s || dci == 0)
        return false;
    if (dci == 1) {
        s->ctrl_cc = cc;
        s->ctrl_done = true;
        return true;
    }
    struct endpoint *ep = &s->ep[dci];
    if (ep->bulk) {
        ep->cc = cc;
        ep->residual = residual;
        ep->done = true;
        return true;
    }
    if (!ep->active)
        return false;
    if (cc == CC_SUCCESS || cc == CC_SHORT_PACKET) {
        ep->failures = 0;
        ep->fn(ep->arg, ep->buf, residual <= ep->len ? ep->len - residual : 0);
        queue_interrupt(x, s, dci);
        /* The report of a hub names ports with a change, which the
         * controller thread handles. */
        if (s->dev.hub) {
            x->hub_work = true;
            return true;
        }
        return false;
    }
    ep->active = false;
    ep->halted = true;
    ep->failures++;
    x->ep_work = true;
    klog_warn("slot %u endpoint %u: completion code %u", slot, dci, cc);
    return true;
}

/* Consume the event ring up to the first TRB that the controller has not
 * written, and move the dequeue pointer past it. The caller has acquired
 * x->lock. */
static void process_events(struct xhci *x)
{
    bool consumed = false, wake = false;
    for (;;) {
        struct trb *e = &x->events[x->ev_dequeue];
        uint32_t control = __atomic_load_n(&e->control, __ATOMIC_ACQUIRE);
        if ((control & TRB_CYCLE) != x->ev_cycle)
            break;
        uint64_t param = e->param;
        uint32_t status = e->status;
        switch ((control >> 10) & 0x3f) {
        case TRB_COMMAND_COMPLETION:
            if (param == x->cmd_trb) {
                x->cmd_cc = status >> 24;
                x->cmd_slot = control >> 24;
                x->cmd_done = true;
                wake = true;
            }
            break;
        case TRB_TRANSFER_EVENT:
            wake |= transfer_event(x, status, control);
            break;
        case TRB_PORT_STATUS_CHANGE: {
            unsigned port = (unsigned)(param >> 24) & 0xff;
            if (port >= 1 && port <= x->max_ports) {
                x->port_pending[port / 64] |= 1UL << (port % 64);
                wake = true;
            }
            break;
        }
        default:
            break;
        }
        if (++x->ev_dequeue == RING_TRBS) {
            x->ev_dequeue = 0;
            x->ev_cycle ^= 1;
        }
        consumed = true;
    }
    if (consumed)
        wr64(x->rt, RT_ERDP, (x->events_phys + x->ev_dequeue * sizeof(struct trb)) | ERDP_EHB);
    if (wake)
        waitq_wake_all(&x->waitq);
}

static void xhci_irq(struct trapframe *tf, void *arg)
{
    struct xhci *x = arg;
    spin_lock(&x->lock);
    wr(x->op, OP_USBSTS, USBSTS_EINT);
    wr(x->rt, RT_IMAN, IMAN_IE | IMAN_IP);
    process_events(x);
    spin_unlock(&x->lock);
}

/* Wait until *flag is set, the device of gone is disconnected or the
 * timeout ends. The events are processed on every pass, which completes
 * the wait also when no interrupt arrives. The caller has acquired
 * x->lock. The wait releases it while it sleeps. */
static int wait_flag(struct xhci *x, bool *flag, const bool *gone, uint64_t timeout_ms)
{
    uint64_t end = timer_ms() + timeout_ms;
    for (;;) {
        process_events(x);
        if (*flag)
            return 0;
        if (gone && *gone)
            return -ENODEV;
        uint64_t now = timer_ms();
        if (now >= end)
            return -ETIMEDOUT;
        waitq_wait_timeout(&x->waitq, &x->lock, MIN(end, now + POLL_MS));
    }
}

/* Run one command and wait for its completion. cmd_lock serializes the
 * commands of the controller thread and of the class drivers. */
static int command(struct xhci *x, uint64_t param, uint32_t control, unsigned *slot_out)
{
    mutex_lock(&x->cmd_lock);
    spin_lock(&x->lock);
    x->cmd_done = false;
    x->cmd_trb = ring_push(&x->cmd, param, 0, control);
    doorbell(x, 0, 0);
    int r = wait_flag(x, &x->cmd_done, NULL, CMD_TIMEOUT_MS);
    uint32_t cc = x->cmd_cc;
    unsigned slot = x->cmd_slot;
    spin_unlock(&x->lock);
    mutex_unlock(&x->cmd_lock);
    unsigned type = (control >> 10) & 0x3f;
    if (r < 0) {
        klog_error("command %u: no completion within %u ms", type, CMD_TIMEOUT_MS);
        return r;
    }
    if (cc != CC_SUCCESS) {
        klog_warn("command %u: completion code %u", type, cc);
        return -EIO;
    }
    if (slot_out)
        *slot_out = slot;
    return 0;
}

/* Return an endpoint to the running state with its ring empty: Reset
 * Endpoint for a halted endpoint, Stop Endpoint for one that did not
 * complete, then Set TR Dequeue Pointer to the enqueue position. */
static int recover_endpoint(struct xhci *x, struct slot *s, unsigned dci, bool halted)
{
    uint32_t target = TRB_SLOT(s->dev.slot) | TRB_EP(dci);
    if (halted)
        command(x, 0, TRB_TYPE(TRB_RESET_EP) | target, NULL);
    else
        command(x, 0, TRB_TYPE(TRB_STOP_EP) | target, NULL);
    spin_lock(&x->lock);
    uint64_t deq = ring_dequeue_pointer(&s->ep[dci].ring);
    spin_unlock(&x->lock);
    return command(x, deq, TRB_TYPE(TRB_SET_TR_DEQUEUE) | target, NULL);
}

int xhci_control(struct usb_device *dev, uint8_t request_type, uint8_t request, uint16_t value,
                 uint16_t index, void *data, uint16_t len)
{
    struct xhci *x = dev->hc;
    struct slot *s = container_of(dev, struct slot, dev);
    if (len > PAGE_SIZE)
        return -EINVAL;
    bool in = request_type & USB_DIR_IN;
    uint64_t setup = request_type | (uint64_t)request << 8 | (uint64_t)value << 16 |
                     (uint64_t)index << 32 | (uint64_t)len << 48;
    uint32_t trt = len == 0 ? 0 : in ? 3 : 2;   /* no data, OUT data, IN data */
    struct ring *ring = &s->ep[1].ring;
    mutex_lock(&s->ctrl_lock);
    if (!in && len)
        memcpy(s->ctrl_buf, data, len);
    spin_lock(&x->lock);
    if (s->dev.gone) {
        spin_unlock(&x->lock);
        mutex_unlock(&s->ctrl_lock);
        return -ENODEV;
    }
    s->ctrl_done = false;
    ring_push(ring, setup, 8, TRB_TYPE(TRB_SETUP) | TRB_IDT | trt << 16);
    if (len)
        ring_push(ring, s->ctrl_phys, len, TRB_TYPE(TRB_DATA) | (in ? TRB_DIR_IN : 0));
    /* The status stage goes in the direction opposite to the data. */
    ring_push(ring, 0, 0, TRB_TYPE(TRB_STATUS) | TRB_IOC | (len && in ? 0 : TRB_DIR_IN));
    doorbell(x, dev->slot, 1);
    int err = wait_flag(x, &s->ctrl_done, &s->dev.gone, CTRL_TIMEOUT_MS);
    uint32_t cc = s->ctrl_cc;
    spin_unlock(&x->lock);
    int r = len;
    if (err == -ENODEV) {
        r = err;
    } else if (err < 0) {
        klog_warn("slot %u: control request %02x:%02x without completion", dev->slot, request_type, request);
        recover_endpoint(x, s, 1, false);
        r = err;
    } else if (cc == CC_STALL) {
        recover_endpoint(x, s, 1, true);
        r = -EPIPE;
    } else if (cc != CC_SUCCESS && cc != CC_SHORT_PACKET) {
        r = -EIO;
    } else if (in && len) {
        memcpy(data, s->ctrl_buf, len);
    }
    mutex_unlock(&s->ctrl_lock);
    return r;
}

/* Configure the endpoint of index dci with the endpoint context words
 * epc0, epc1 and epc4 through Configure Endpoint. The slot context comes from
 * the device context, with the last valid context index raised. */
static int configure_endpoint(struct xhci *x, struct slot *s, unsigned dci, uint32_t epc0, uint32_t epc1,
                              uint32_t epc4)
{
    struct endpoint *ep = &s->ep[dci];
    memset(s->in_ctx, 0, PAGE_SIZE);
    in_ctx(x, s, 0)[1] = 1u | 1u << dci;
    memcpy(in_ctx(x, s, 1), out_ctx(x, s, 0), x->ctx_size);
    if (dci > s->max_dci)
        s->max_dci = dci;
    uint32_t *slotctx = in_ctx(x, s, 1);
    slotctx[0] = (slotctx[0] & ~(0x1fu << 27)) | (uint32_t)s->max_dci << 27;
    uint32_t *epc = in_ctx(x, s, dci + 1);
    epc[0] = epc0;
    epc[1] = epc1;
    uint64_t deq = ep->ring.phys | 1;
    epc[2] = (uint32_t)deq;
    epc[3] = (uint32_t)(deq >> 32);
    epc[4] = epc4;
    return command(x, s->in_phys, TRB_TYPE(TRB_CONFIGURE_EP) | TRB_SLOT(s->dev.slot), NULL);
}

static unsigned ep_dci(uint8_t ep_addr)
{
    return (ep_addr & 0xf) * 2u + ((ep_addr & USB_DIR_IN) ? 1u : 0u);
}

/* The Interval field of an endpoint context: the period is 2^Interval
 * units of 125 us. bInterval counts frames of 1 ms at full and low speed,
 * and is an exponent of 125 us units, plus 1, at high and super speed. */
static unsigned endpoint_interval(enum usb_speed speed, uint8_t binterval)
{
    if (speed == USB_SPEED_HIGH || speed == USB_SPEED_SUPER)
        return binterval >= 1 && binterval <= 16 ? binterval - 1u : 3;
    unsigned units = (binterval ? binterval : 1) * 8u, e = 0;
    while ((2u << e) <= units)
        e++;
    return e < 3 ? 3 : e > 10 ? 10 : e;
}

int xhci_interrupt_in(struct usb_device *dev, const struct usb_endpoint_descriptor *epd, unsigned len,
                      usb_report_fn fn, void *arg)
{
    struct xhci *x = dev->hc;
    struct slot *s = container_of(dev, struct slot, dev);
    unsigned num = epd->bEndpointAddress & 0xf;
    if (num == 0 || !(epd->bEndpointAddress & USB_DIR_IN))
        return -EINVAL;
    unsigned dci = num * 2 + 1;
    struct endpoint *ep = &s->ep[dci];
    if (ep->ring.trb)
        return -EBUSY;
    unsigned mps = epd->wMaxPacketSize & 0x7ff;
    if (ring_init(&ep->ring) < 0 || !(ep->buf = pmm_alloc_dma_page(&ep->buf_phys)))
        return -ENOMEM;
    ep->len = MIN(MAX(len, mps), 1024u);

    /* The average TRB length and the maximum ESIT payload are one packet. */
    int r = configure_endpoint(x, s, dci, endpoint_interval(dev->speed, epd->bInterval) << 16,
                               3u << 1 | EP_TYPE_INT_IN << 3 | mps << 16, mps | mps << 16);
    if (r < 0)
        return r;
    spin_lock(&x->lock);
    ep->fn = fn;
    ep->arg = arg;
    ep->active = true;
    queue_interrupt(x, s, dci);
    spin_unlock(&x->lock);
    return 0;
}

int xhci_bulk_open(struct usb_device *dev, const struct usb_endpoint_descriptor *epd, unsigned max_burst)
{
    struct xhci *x = dev->hc;
    struct slot *s = container_of(dev, struct slot, dev);
    unsigned dci = ep_dci(epd->bEndpointAddress);
    if ((epd->bEndpointAddress & 0xf) == 0 || (epd->bmAttributes & USB_EP_XFER_MASK) != USB_EP_XFER_BULK)
        return -EINVAL;
    struct endpoint *ep = &s->ep[dci];
    if (ep->ring.trb)
        return -EBUSY;
    if (ring_init(&ep->ring) < 0 || !(ep->bulk_pages = pmm_alloc(BULK_ORDER)))
        return -ENOMEM;
    ep->buf = phys_to_virt(page_to_phys(ep->bulk_pages));
    ep->buf_phys = page_to_phys(ep->bulk_pages);
    unsigned mps = epd->wMaxPacketSize & 0x7ff;
    unsigned type = (epd->bEndpointAddress & USB_DIR_IN) ? EP_TYPE_BULK_IN : EP_TYPE_BULK_OUT;
    /* The average TRB length of a bulk endpoint is 3 KiB, as in edk2. */
    int r = configure_endpoint(x, s, dci, 0, 3u << 1 | type << 3 | (max_burst & 0xff) << 8 | mps << 16, 3072);
    if (r < 0)
        return r;
    spin_lock(&x->lock);
    ep->bulk = true;
    spin_unlock(&x->lock);
    return 0;
}

int xhci_bulk(struct usb_device *dev, uint8_t ep_addr, void *buf, uint32_t len, unsigned timeout_ms,
              uint32_t *actual)
{
    struct xhci *x = dev->hc;
    struct slot *s = container_of(dev, struct slot, dev);
    unsigned dci = ep_dci(ep_addr);
    struct endpoint *ep = &s->ep[dci];
    bool in = ep_addr & USB_DIR_IN;
    *actual = 0;
    if (dci >= MAX_DCI || !ep->bulk || len > XHCI_BULK_MAX)
        return -EINVAL;
    if (!in && len)
        memcpy(ep->buf, buf, len);
    spin_lock(&x->lock);
    if (s->dev.gone) {
        spin_unlock(&x->lock);
        return -ENODEV;
    }
    ep->done = false;
    ring_push(&ep->ring, ep->buf_phys, len, TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP);
    doorbell(x, dev->slot, dci);
    int err = wait_flag(x, &ep->done, &s->dev.gone, timeout_ms);
    uint32_t cc = ep->cc, residual = ep->residual;
    spin_unlock(&x->lock);
    if (err == -ENODEV)
        return err;
    if (err < 0) {
        klog_warn("slot %u: bulk transfer on endpoint %02x without completion", dev->slot, ep_addr);
        recover_endpoint(x, s, dci, false);
        return err;
    }
    if (cc == CC_STALL)
        return -EPIPE;
    if (cc != CC_SUCCESS && cc != CC_SHORT_PACKET) {
        klog_warn("slot %u: bulk transfer on endpoint %02x: completion code %u", dev->slot, ep_addr, cc);
        recover_endpoint(x, s, dci, true);
        return -EIO;
    }
    uint32_t n = residual <= len ? len - residual : 0;
    if (in && n)
        memcpy(buf, ep->buf, n);
    *actual = n;
    return 0;
}

int xhci_clear_halt(struct usb_device *dev, uint8_t ep_addr)
{
    struct xhci *x = dev->hc;
    struct slot *s = container_of(dev, struct slot, dev);
    unsigned dci = ep_dci(ep_addr);
    if (dci >= MAX_DCI || !s->ep[dci].ring.trb)
        return -EINVAL;
    recover_endpoint(x, s, dci, true);
    int r = xhci_control(dev, USB_RECIP_ENDPOINT, USB_REQ_CLEAR_FEATURE, USB_FEATURE_ENDPOINT_HALT, ep_addr,
                         NULL, 0);
    return r < 0 ? r : 0;
}

int xhci_hub_configure(struct usb_device *dev, unsigned nports, unsigned ttt, bool mtt)
{
    struct xhci *x = dev->hc;
    struct slot *s = container_of(dev, struct slot, dev);
    memset(s->in_ctx, 0, PAGE_SIZE);
    in_ctx(x, s, 0)[1] = 1u;
    memcpy(in_ctx(x, s, 1), out_ctx(x, s, 0), x->ctx_size);
    uint32_t *slotctx = in_ctx(x, s, 1);
    slotctx[0] |= 1u << 26 | (mtt ? 1u << 25 : 0);
    slotctx[1] = (slotctx[1] & 0x00ffffffu) | (uint32_t)MIN(nports, 255u) << 24;
    slotctx[2] = (slotctx[2] & ~(3u << 16)) | (ttt & 3u) << 16;
    return command(x, s->in_phys, TRB_TYPE(TRB_CONFIGURE_EP) | TRB_SLOT(dev->slot), NULL);
}

static void slot_free(struct slot *s)
{
    for (unsigned i = 0; i < MAX_DCI; i++) {
        pmm_free_dma_page(s->ep[i].ring.trb);
        if (s->ep[i].bulk_pages)
            pmm_free(s->ep[i].bulk_pages, BULK_ORDER);
        else
            pmm_free_dma_page(s->ep[i].buf);
    }
    pmm_free_dma_page(s->out_ctx);
    pmm_free_dma_page(s->in_ctx);
    pmm_free_dma_page(s->ctrl_buf);
    kfree(s->dev.config);
    kfree(s);
}

/* Give a device an address and enumerate it. parent is NULL for a device
 * on a root port and the hub otherwise, and port is the port of the root
 * hub or of the parent hub. The slot context of a device behind a hub
 * carries the route string, the root port and the transaction translator
 * of a low or full speed device behind a high speed hub
 * (XhcInitializeDeviceSlot of edk2). Returns the device, or NULL. */
static struct usb_device *attach(struct xhci *x, struct usb_device *parent, unsigned port, enum usb_speed speed)
{
    unsigned slot;
    if (command(x, 0, TRB_TYPE(TRB_ENABLE_SLOT), &slot) < 0)
        return NULL;
    if (slot < 1 || slot > x->max_slots) {
        klog_error("port %u: slot %u out of range", port, slot);
        return NULL;
    }
    struct slot *s = kzalloc(sizeof *s);
    if (!s || !(s->out_ctx = pmm_alloc_dma_page(&s->out_phys)) || !(s->in_ctx = pmm_alloc_dma_page(&s->in_phys)) ||
        !(s->ctrl_buf = pmm_alloc_dma_page(&s->ctrl_phys)) || ring_init(&s->ep[1].ring) < 0) {
        klog_error("port %u: no memory for slot %u", port, slot);
        if (s)
            slot_free(s);
        command(x, 0, TRB_TYPE(TRB_DISABLE_SLOT) | TRB_SLOT(slot), NULL);
        return NULL;
    }
    mutex_init(&s->ctrl_lock, "xhci_ctrl");
    s->dev.hc = x;
    s->dev.slot = slot;
    s->dev.speed = speed;
    if (parent) {
        s->dev.parent = parent;
        s->dev.parent_port = port;
        s->dev.port = parent->port;
        s->dev.depth = parent->depth + 1;
        s->dev.route = parent->route | (uint32_t)MIN(port, 15u) << (4 * parent->depth);
        if ((speed == USB_SPEED_LOW || speed == USB_SPEED_FULL) && parent->speed == USB_SPEED_HIGH) {
            s->dev.tt_slot = parent->slot;
            s->dev.tt_port = port;
        } else {
            s->dev.tt_slot = parent->tt_slot;
            s->dev.tt_port = parent->tt_port;
        }
        ksnprintf(s->dev.path, sizeof s->dev.path, "%s.%u", parent->path, port);
    } else {
        s->dev.port = port;
        ksnprintf(s->dev.path, sizeof s->dev.path, "%u", port);
    }
    s->max_dci = 1;
    x->dcbaa[slot] = s->out_phys;
    spin_lock(&x->lock);
    x->slots[slot] = s;
    spin_unlock(&x->lock);

    /* Address Device with the slot context and endpoint 0. The packet size
     * of endpoint 0 is the largest one the speed allows, or 8 at full speed,
     * until the device descriptor states it. */
    unsigned mps = s->dev.speed == USB_SPEED_SUPER ? 512 : s->dev.speed == USB_SPEED_HIGH ? 64 : 8;
    in_ctx(x, s, 0)[1] = 0x3;
    in_ctx(x, s, 1)[0] = s->dev.route | (uint32_t)s->dev.speed << 20 | 1u << 27;
    in_ctx(x, s, 1)[1] = s->dev.port << 16;
    in_ctx(x, s, 1)[2] = s->dev.tt_slot | s->dev.tt_port << 8;
    uint32_t *ep0 = in_ctx(x, s, 2);
    ep0[1] = 3u << 1 | EP_TYPE_CONTROL << 3 | mps << 16;
    uint64_t deq = s->ep[1].ring.phys | 1;
    ep0[2] = (uint32_t)deq;
    ep0[3] = (uint32_t)(deq >> 32);
    ep0[4] = 8;
    if (command(x, s->in_phys, TRB_TYPE(TRB_ADDRESS_DEVICE) | TRB_SLOT(slot), NULL) < 0)
        goto fail;
    sleep_ms(10);                               /* the recovery interval after SET_ADDRESS */

    uint8_t head[8];
    if (xhci_control(&s->dev, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR, USB_DT_DEVICE << 8, 0, head, 8) < 0) {
        klog_warn("port %s: no device descriptor", s->dev.path);
        goto fail;
    }
    unsigned real = s->dev.speed == USB_SPEED_SUPER ? 1u << head[7] : head[7];
    if (real && real != mps) {
        memset(s->in_ctx, 0, PAGE_SIZE);
        in_ctx(x, s, 0)[1] = 0x2;
        in_ctx(x, s, 2)[1] = 3u << 1 | EP_TYPE_CONTROL << 3 | real << 16;
        if (command(x, s->in_phys, TRB_TYPE(TRB_EVALUATE_CONTEXT) | TRB_SLOT(slot), NULL) < 0)
            goto fail;
    }
    usb_count_device(1);
    usb_enumerate(&s->dev);
    return &s->dev;
fail:
    spin_lock(&x->lock);
    x->slots[slot] = NULL;
    spin_unlock(&x->lock);
    command(x, 0, TRB_TYPE(TRB_DISABLE_SLOT) | TRB_SLOT(slot), NULL);
    x->dcbaa[slot] = 0;
    slot_free(s);
    return NULL;
}

struct usb_device *xhci_attach(struct usb_device *parent, unsigned port, enum usb_speed speed)
{
    return attach(parent->hc, parent, port, speed);
}

/* Give the device on a root port an address, enumerate it and publish
 * it. */
static void port_attach(struct xhci *x, unsigned port, uint32_t portsc)
{
    struct usb_device *dev = attach(x, NULL, port, (enum usb_speed)PORTSC_SPEED(portsc));
    if (!dev)
        return;
    mutex_lock(&usb_topology_lock);
    x->port_slot[port] = container_of(dev, struct slot, dev);
    mutex_unlock(&usb_topology_lock);
}

/* Remove a disconnected device. Its transfers end with ENODEV from the
 * moment it is marked gone, and its events are ignored from the moment
 * its slot leaves slots[]. The class drivers unbind first. A hub removes
 * the devices behind it, and mass storage waits for its last command. */
void xhci_detach(struct usb_device *dev)
{
    struct xhci *x = dev->hc;
    struct slot *s = container_of(dev, struct slot, dev);
    spin_lock(&x->lock);
    s->dev.gone = true;
    for (unsigned i = 0; i < MAX_DCI; i++)
        s->ep[i].active = false;
    waitq_wake_all(&x->waitq);
    spin_unlock(&x->lock);
    klog_info("port %s: %s disconnected", s->dev.path, s->dev.product);
    usb_disconnect(&s->dev);
    spin_lock(&x->lock);
    x->slots[s->dev.slot] = NULL;
    spin_unlock(&x->lock);
    command(x, 0, TRB_TYPE(TRB_DISABLE_SLOT) | TRB_SLOT(s->dev.slot), NULL);
    x->dcbaa[s->dev.slot] = 0;
    usb_count_device(-1);
    slot_free(s);
}

/* Remove the device of a disconnected root port. */
static void port_detach(struct xhci *x, unsigned port)
{
    struct slot *s = x->port_slot[port];
    mutex_lock(&usb_topology_lock);
    x->port_slot[port] = NULL;
    mutex_unlock(&usb_topology_lock);
    xhci_detach(&s->dev);
}

static void port_clear_changes(struct xhci *x, unsigned port, uint32_t portsc)
{
    wr(x->op, OP_PORTSC(port), (portsc & PORTSC_PRESERVE) | (portsc & PORTSC_CHANGES));
}

/* Wait up to ms milliseconds until the port register has every bit of
 * mask set. Returns the last value read. */
static uint32_t port_wait(struct xhci *x, unsigned port, uint32_t mask, unsigned ms)
{
    uint32_t v = rd(x->op, OP_PORTSC(port));
    for (unsigned t = 0; t < ms && (v & mask) != mask; t += 10) {
        sleep_ms(10);
        v = rd(x->op, OP_PORTSC(port));
    }
    return v;
}

/* Enable the port of a connected device. A USB 2 port is enabled by a
 * reset. A USB 3 port enables itself when its link trains, and a warm
 * reset trains a link that did not. */
static int port_enable(struct xhci *x, unsigned port)
{
    uint32_t v = rd(x->op, OP_PORTSC(port));
    if (x->port_major[port] >= 3) {
        v = port_wait(x, port, PORTSC_PED, 200);
        if (!(v & PORTSC_PED)) {
            wr(x->op, OP_PORTSC(port), (v & PORTSC_PRESERVE) | PORTSC_WPR);
            v = port_wait(x, port, PORTSC_WRC, 500);
            v = port_wait(x, port, PORTSC_PED, 200);
        }
    } else {
        wr(x->op, OP_PORTSC(port), (v & PORTSC_PRESERVE) | PORTSC_PR);
        v = port_wait(x, port, PORTSC_PRC, 500);
        if (!(v & PORTSC_PED))
            v = port_wait(x, port, PORTSC_PED, 100);
    }
    port_clear_changes(x, port, rd(x->op, OP_PORTSC(port)));
    return v & PORTSC_PED ? 0 : -EIO;
}

/* Bring the device on a port in line with the port status: detach a device
 * that is gone or was replaced, attach a newly connected one. */
static void handle_port(struct xhci *x, unsigned port)
{
    uint32_t v = rd(x->op, OP_PORTSC(port));
    port_clear_changes(x, port, v);
    bool connected = v & PORTSC_CCS;
    if (x->port_slot[port] && (!connected || (v & PORTSC_CSC)))
        port_detach(x, port);
    if (!connected || x->port_slot[port])
        return;
    sleep_ms(100);                              /* the connect debounce interval */
    v = rd(x->op, OP_PORTSC(port));
    if (!(v & PORTSC_CCS))
        return;
    if (port_enable(x, port) < 0) {
        klog_warn("port %u: connected, but the port does not enable", port);
        return;
    }
    port_attach(x, port, rd(x->op, OP_PORTSC(port)));
}

/* Reset the interrupt endpoints that failed and queue their transfers
 * again, up to EP_RETRIES consecutive failures. */
static void recover_halted(struct xhci *x)
{
    for (unsigned slot = 1; slot <= MAX_SLOTS; slot++) {
        spin_lock(&x->lock);
        struct slot *s = x->slots[slot];
        spin_unlock(&x->lock);
        if (!s)
            continue;
        for (unsigned dci = 2; dci < MAX_DCI; dci++) {
            struct endpoint *ep = &s->ep[dci];
            spin_lock(&x->lock);
            bool halted = ep->halted;
            ep->halted = false;
            unsigned failures = ep->failures;
            spin_unlock(&x->lock);
            if (!halted)
                continue;
            if (failures > EP_RETRIES) {
                klog_error("slot %u endpoint %u: failed %u times, stopped", slot, dci, failures);
                continue;
            }
            if (recover_endpoint(x, s, dci, true) < 0)
                continue;
            spin_lock(&x->lock);
            if (x->slots[slot] == s) {
                ep->active = true;
                queue_interrupt(x, s, dci);
            }
            spin_unlock(&x->lock);
        }
    }
}

/* Handle the port changes of every hub. Only this thread frees slots, so a
 * slot read from slots[] remains valid while the thread uses it. */
static void service_hubs(struct xhci *x)
{
    for (unsigned slot = 1; slot <= MAX_SLOTS; slot++) {
        spin_lock(&x->lock);
        struct slot *s = x->slots[slot];
        spin_unlock(&x->lock);
        if (s && s->dev.hub && !s->dev.gone)
            hub_service(&s->dev);
    }
}

/* The controller thread: the ports present at boot, then every port status
 * change and every halted endpoint. Without an interrupt it also polls the
 * event ring. */
static void xhci_thread(void *arg)
{
    struct xhci *x = arg;
    for (unsigned port = 1; port <= x->max_ports; port++)
        handle_port(x, port);
    spin_lock(&x->lock);
    x->initial_done = true;
    waitq_wake_all(&x->waitq);
    spin_unlock(&x->lock);
    for (;;) {
        uint64_t pending[MAX_PORTS / 64];
        bool work = false, any = false;
        spin_lock(&x->lock);
        for (;;) {
            process_events(x);
            for (unsigned i = 0; i < MAX_PORTS / 64; i++)
                any |= x->port_pending[i] != 0;
            if (any || x->ep_work || x->hub_work)
                break;
            waitq_wait_timeout(&x->waitq, &x->lock, timer_ms() + (x->irq >= 0 ? 1000 : POLL_MS));
        }
        memcpy(pending, x->port_pending, sizeof pending);
        memset(x->port_pending, 0, sizeof x->port_pending);
        work = x->ep_work;
        x->ep_work = false;
        bool hubs = x->hub_work;
        x->hub_work = false;
        spin_unlock(&x->lock);
        for (unsigned port = 1; port <= x->max_ports; port++)
            if (pending[port / 64] & (1UL << (port % 64)))
                handle_port(x, port);
        if (work)
            recover_halted(x);
        if (hubs)
            service_hubs(x);
    }
}

/* Take the controller from the firmware (USB legacy support capability)
 * and record the USB major revision of every port (supported protocol
 * capabilities). */
static void read_extended_capabilities(struct xhci *x, unsigned xecp)
{
    for (unsigned off = xecp, guard = 0; off && guard < 64; guard++) {
        uint32_t v = rd(x->cap, off);
        if ((v & 0xff) == XCAP_LEGACY) {
            if (v & LEGACY_BIOS_OWNED) {
                wr(x->cap, off, v | LEGACY_OS_OWNED);
                for (unsigned t = 0; t < 1000 && (rd(x->cap, off) & LEGACY_BIOS_OWNED); t += 10)
                    sleep_ms(10);
                if (rd(x->cap, off) & LEGACY_BIOS_OWNED)
                    klog_warn("the firmware did not release the controller");
            }
            /* The SMI enables off, the SMI status bits cleared. */
            wr(x->cap, off + 4, 0xe0000000u);
        } else if ((v & 0xff) == XCAP_PROTOCOL) {
            unsigned major = v >> 24;
            uint32_t ports = rd(x->cap, off + 8);
            unsigned first = ports & 0xff, count = (ports >> 8) & 0xff;
            for (unsigned p = first; p < first + count && p <= MAX_PORTS; p++)
                x->port_major[p] = (uint8_t)major;
        }
        unsigned next = (v >> 8) & 0xff;
        if (!next)
            break;
        off += next * 4;
    }
}

/* Stop and reset the controller. */
static int controller_reset(struct xhci *x)
{
    uint32_t cmd = rd(x->op, OP_USBCMD);
    if (cmd & USBCMD_RS) {
        wr(x->op, OP_USBCMD, cmd & ~USBCMD_RS);
        for (unsigned t = 0; t < 100 && !(rd(x->op, OP_USBSTS) & USBSTS_HCH); t++)
            sleep_ms(1);
    }
    wr(x->op, OP_USBCMD, USBCMD_HCRST);
    for (unsigned t = 0; t < 1000; t++) {
        if (!(rd(x->op, OP_USBCMD) & USBCMD_HCRST) && !(rd(x->op, OP_USBSTS) & USBSTS_CNR))
            return 0;
        sleep_ms(1);
    }
    return -ETIMEDOUT;
}

/* The scratchpad buffers that the controller asks for, and their array in
 * entry 0 of the device context array. */
static int setup_scratchpad(struct xhci *x)
{
    uint32_t hcs2 = rd(x->cap, CAP_HCSPARAMS2);
    unsigned count = ((hcs2 >> 21) & 0x1f) << 5 | ((hcs2 >> 27) & 0x1f);
    if (!count)
        return 0;
    if (count > PAGE_SIZE / 8)
        return -EINVAL;
    x->scratch_count = count;
    uintptr_t phys;
    x->scratch = pmm_alloc_dma_page(&phys);
    if (!x->scratch)
        return -ENOMEM;
    for (unsigned i = 0; i < count; i++) {
        uintptr_t p;
        if (!pmm_alloc_dma_page(&p))
            return -ENOMEM;
        x->scratch[i] = p;
    }
    x->dcbaa[0] = phys;
    return 0;
}

/* The interrupt of the controller: MSI-X, else MSI, else none, and the
 * thread polls. */
static void setup_interrupt(struct xhci *x)
{
    x->irq = -1;
    x->irq_kind = "no";
    int irq = irq_alloc();
    if (irq < 0)
        return;
    irq_register((unsigned)irq, xhci_irq, x);
    if (pci_msix_enable(x->pci) == 0 && pci_msix_set_vector(x->pci, 0, (unsigned)irq) == 0) {
        x->irq = irq;
        x->irq_kind = "msi-x";
    } else if (pci_msi_enable(x->pci, (unsigned)irq) == 0) {
        x->irq = irq;
        x->irq_kind = "msi";
    }
}

static struct xhci *xhci_start(struct pci_dev *p, unsigned index)
{
    uint64_t size = pci_bar_size(p, 0);
    if (!size) {
        klog_error("%02x:%02x.%u: no memory BAR", p->bus, p->slot, p->func);
        return NULL;
    }
    struct xhci *x = kzalloc(sizeof *x);
    if (!x)
        return NULL;
    x->pci = p;
    x->index = index;
    spinlock_init(&x->lock, "xhci");
    mutex_init(&x->cmd_lock, "xhci_cmd");
    waitq_init(&x->waitq, "xhci");
    pci_enable_bus_master(p);
    x->cap = vmm_map_mmio(p->bar[0], ALIGN_UP(size, PAGE_SIZE), VM_KERNEL_RW | VM_NOCACHE);
    if (!x->cap)
        goto fail;
    x->op = x->cap + (rd(x->cap, CAP_CAPLENGTH) & 0xff);
    x->rt = x->cap + (rd(x->cap, CAP_RTSOFF) & ~0x1fu);
    x->db = x->cap + (rd(x->cap, CAP_DBOFF) & ~0x3u);
    uint32_t hcs1 = rd(x->cap, CAP_HCSPARAMS1);
    uint32_t hcc1 = rd(x->cap, CAP_HCCPARAMS1);
    unsigned version = rd(x->cap, CAP_CAPLENGTH) >> 16;
    x->version = version;
    x->max_slots = MIN(hcs1 & 0xff, (unsigned)MAX_SLOTS);
    x->max_ports = MIN(hcs1 >> 24, (unsigned)MAX_PORTS);
    x->ctx_size = (hcc1 & (1u << 2)) ? 64 : 32;
    if (!(hcc1 & 1)) {
        klog_error("%02x:%02x.%u: the controller addresses 32 bits only, not supported",
                   p->bus, p->slot, p->func);
        goto fail;
    }
    for (unsigned port = 1; port <= x->max_ports; port++)
        x->port_major[port] = 2;
    read_extended_capabilities(x, (hcc1 >> 16) * 4);
    if (controller_reset(x) < 0) {
        klog_error("%02x:%02x.%u: reset did not complete", p->bus, p->slot, p->func);
        goto fail;
    }
    if (!(rd(x->op, OP_PAGESIZE) & 1)) {
        klog_error("%02x:%02x.%u: 4 KiB pages not supported", p->bus, p->slot, p->func);
        goto fail;
    }
    wr(x->op, OP_CONFIG, x->max_slots);
    x->dcbaa = pmm_alloc_dma_page(&x->dcbaa_phys);
    if (!x->dcbaa || setup_scratchpad(x) < 0 || ring_init(&x->cmd) < 0)
        goto fail;
    wr64(x->op, OP_DCBAAP, x->dcbaa_phys);
    wr64(x->op, OP_CRCR, x->cmd.phys | 1);

    /* One event ring segment of one page for interrupter 0. ERSTBA is
     * written last, because writing it makes the controller read the
     * segment table. */
    x->events = pmm_alloc_dma_page(&x->events_phys);
    x->erst = pmm_alloc_dma_page(&x->erst_phys);
    if (!x->events || !x->erst)
        goto fail;
    x->erst[0] = x->events_phys;
    x->erst[1] = RING_TRBS;
    x->ev_cycle = 1;
    wr(x->rt, RT_ERSTSZ, 1);
    wr64(x->rt, RT_ERDP, x->events_phys);
    wr64(x->rt, RT_ERSTBA, x->erst_phys);
    wr(x->rt, RT_IMOD, 4000);                   /* at most one interrupt per ms */
    setup_interrupt(x);
    wr(x->rt, RT_IMAN, IMAN_IE | IMAN_IP);
    wr(x->op, OP_USBCMD, USBCMD_RS | (x->irq >= 0 ? USBCMD_INTE : 0));
    for (unsigned t = 0; t < 100 && (rd(x->op, OP_USBSTS) & USBSTS_HCH); t++)
        sleep_ms(1);
    if (rd(x->op, OP_USBSTS) & USBSTS_HCH) {
        klog_error("%02x:%02x.%u: the controller does not run", p->bus, p->slot, p->func);
        goto fail;
    }
    /* Ports with power switches are off after the reset. */
    bool powered = false;
    for (unsigned port = 1; port <= x->max_ports; port++) {
        uint32_t v = rd(x->op, OP_PORTSC(port));
        if (!(v & PORTSC_PP)) {
            wr(x->op, OP_PORTSC(port), (v & PORTSC_PRESERVE) | PORTSC_PP);
            powered = true;
        }
    }
    if (powered)
        sleep_ms(20);
    klog_info("%02x:%02x.%u: xhci %x.%02x, %s interrupt, %u ports, %u slots, %u byte contexts", p->bus,
              p->slot, p->func, version >> 8, version & 0xff, x->irq_kind, x->max_ports, x->max_slots,
              x->ctx_size);
    char name[16];
    ksnprintf(name, sizeof name, "xhci%u", index);
    if (!thread_create(name, xhci_thread, x, 0)) {
        klog_error("cannot start the controller thread");
        goto fail;
    }
    return x;
fail:
    /* The pages given to a controller that failed are not returned: the
     * controller may still own some of them. */
    return NULL;
}

void usb_init(void)
{
    mutex_init(&usb_topology_lock, "usb_topology");
    for (size_t i = 0; i < pci_count() && ncontrollers < MAX_CONTROLLERS; i++) {
        struct pci_dev *p = pci_device(i);
        if (p->class != 0x0c || p->subclass != 0x03 || p->prog_if != 0x30)
            continue;
        struct xhci *x = xhci_start(p, ncontrollers);
        if (x) {
            controllers[ncontrollers++] = x;
            p->driver = "xhci";
        }
    }
    /* The devices present at boot are input devices before init starts the
     * display server, which opens the input devices once, and disks before
     * the partition scan. Hubs and mass storage need up to five seconds. */
    uint64_t deadline = timer_ms() + 5000;
    for (unsigned i = 0; i < ncontrollers; i++) {
        struct xhci *x = controllers[i];
        spin_lock(&x->lock);
        while (!x->initial_done && timer_ms() < deadline)
            waitq_wait_timeout(&x->waitq, &x->lock, deadline);
        bool done = x->initial_done;
        spin_unlock(&x->lock);
        if (!done)
            klog_warn("xhci%u: the devices present at boot are not enumerated within 5 s", i);
    }
}

/* ---- /dev/devices (docs/design/sysinfo.md) ---- */

static const char *link_state(unsigned pls)
{
    static const char *const names[16] = {
        "U0 (active)", "U1", "U2", "U3 (suspended)", "disabled", "RxDetect", "inactive", "polling",
        "recovery", "hot reset", "compliance mode", "test mode", NULL, NULL, NULL, "resume",
    };
    return names[pls & 0xf] ? names[pls & 0xf] : "reserved";
}

void usb_describe(struct devinfo *d)
{
    devinfo_node(d, "usb", "USB");
    devinfo_prop(d, "controllers", "%u", ncontrollers);
    devinfo_prop(d, "devices", "%u", usb_device_count());
    if (!ncontrollers)
        return;
    mutex_lock(&usb_topology_lock);
    for (unsigned i = 0; i < ncontrollers; i++) {
        struct xhci *x = controllers[i];
        const struct pci_dev *p = x->pci;
        char path[64];
        ksnprintf(path, sizeof path, "usb/xhci%u", i);
        devinfo_node(d, path, "xHCI controller %02x:%02x.%u", p->bus, p->slot, p->func);
        devinfo_prop(d, "pci_address", "%02x:%02x.%u", p->bus, p->slot, p->func);
        devinfo_prop(d, "interface_version", "%x.%02x", x->version >> 8, x->version & 0xff);
        devinfo_prop(d, "ports", "%u", x->max_ports);
        devinfo_prop(d, "slots", "%u", x->max_slots);
        devinfo_prop(d, "context_size", "%u bytes", x->ctx_size);
        devinfo_prop(d, "interrupt", "%s", x->irq_kind);
        devinfo_prop(d, "scratchpad_buffers", "%u", x->scratch_count);
        for (unsigned port = 1; port <= x->max_ports; port++) {
            uint32_t v = rd(x->op, OP_PORTSC(port));
            struct slot *s = x->port_slot[port];
            char ppath[80];
            ksnprintf(ppath, sizeof ppath, "%s/port%u", path, port);
            devinfo_node(d, ppath, "Port %u, USB %u: %s", port, x->port_major[port],
                         s ? s->dev.product : (v & PORTSC_CCS) ? "device without address" : "empty");
            devinfo_prop(d, "port", "%u", port);
            devinfo_prop(d, "protocol", "USB %u", x->port_major[port]);
            devinfo_prop(d, "connected", "%s", (v & PORTSC_CCS) ? "yes" : "no");
            devinfo_prop(d, "enabled", "%s", (v & PORTSC_PED) ? "yes" : "no");
            devinfo_prop(d, "powered", "%s", (v & PORTSC_PP) ? "yes" : "no");
            if (v & PORTSC_CCS)
                devinfo_prop(d, "speed", "%s", usb_speed_name((enum usb_speed)PORTSC_SPEED(v)));
            devinfo_prop(d, "link_state", "%s", link_state((v >> 5) & 0xf));
            devinfo_prop(d, "over_current", "%s", (v & (1u << 3)) ? "yes" : "no");
            devinfo_prop(d, "portsc", "0x%08x", v);
            if (s) {
                char dpath[96];
                ksnprintf(dpath, sizeof dpath, "%s/device", ppath);
                usb_describe_device(d, dpath, &s->dev);
            }
        }
    }
    mutex_unlock(&usb_topology_lock);
}

