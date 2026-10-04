/* The USB HID class driver (D2, docs/design/usb.md), written from the
 * Device Class Definition for HID 1.11 and the HID Usage Tables 1.4. A
 * boot keyboard uses the boot protocol. Every other HID interface uses the
 * report protocol, and the parser of its report descriptor finds the
 * fields that the input core knows: keys, buttons, relative axes, absolute
 * axes and wheels. Each bound interface reports to one input device. */
#define KLOG_SUBSYS "usb-hid"
#include "usb.h"
#include <drivers/devinfo.h>
#include <input/input.h>
#include <mm/slab.h>
#include <sync/spinlock.h>
#include <lib/list.h>
#include <lib/printf.h>
#include <lib/string.h>
#include <klog.h>
#include <errno.h>

#define HID_REQ_SET_IDLE        0x0a
#define HID_REQ_SET_PROTOCOL    0x0b
#define HID_SUBCLASS_BOOT       1
#define HID_PROTOCOL_KEYBOARD   1

#define PAGE_DESKTOP    0x01
#define PAGE_KEYBOARD   0x07
#define PAGE_BUTTON     0x09
#define PAGE_CONSUMER   0x0c

#define MAX_FIELDS      64      /* input fields of one interface */
#define MAX_REPORTS     8       /* report IDs of one interface */
#define KEY_BITS_BYTES  ((KEY_MAX + 1) / 8)

/* One input field of a report: a variable, which reports one usage, or an
 * array, which reports up to count usages from a range. */
struct hid_field {
    uint8_t report_id;
    bool array;
    bool relative;
    bool is_signed;
    uint8_t bit_size;
    uint8_t count;              /* array: the number of slots; variable: 1 */
    uint16_t bit_offset;
    uint16_t page;
    uint16_t usage;             /* variable: the usage; array: the usage of logical_min */
    int32_t logical_min, logical_max;
    uint16_t type, code;        /* variable: the event; type 0 when unused */
};

/* The key codes that the reports with one ID can set. A report releases
 * only keys of its own ID that it does not set. */
struct hid_report {
    uint8_t id;
    uint8_t mask[KEY_BITS_BYTES];
};

/* A bound HID interface. The fields are written by hid_probe before the
 * interrupt endpoint starts and read by hid_report under xhci.lock.
 * keys[] is written by hid_report only. usb is set by hid_probe and
 * cleared by hid_disconnect after the endpoint stopped. link is in
 * hid_free under hid_free_lock while the interface is disconnected. */
struct hid_dev {
    struct usb_device *usb;
    uint8_t interface;
    bool uses_ids;
    bool boot;                          /* a boot keyboard in the boot protocol */
    unsigned report_bytes;              /* the longest input report, with its ID byte */
    unsigned nfields;
    struct hid_field fields[MAX_FIELDS];
    unsigned nreports;
    struct hid_report reports[MAX_REPORTS];
    uint8_t keys[KEY_BITS_BYTES];       /* the key codes down after the last report */
    struct input_dev input;
    struct list_head link;
};

/* Input devices of disconnected interfaces, for reuse by an interface with
 * the same name and capabilities. hid_free_lock is a leaf. */
static DEFINE_SPINLOCK(hid_free_lock);
static LIST_HEAD(hid_free);

/* The key codes of the keyboard page (0x07), indexed by usage. Usage 0x32,
 * the non-US # key, produces the code of the US backslash key, which is
 * at its position. */
static const uint16_t keyboard_usage[0xe8] = {
    [0x04] = KEY_A, [0x05] = KEY_B, [0x06] = KEY_C, [0x07] = KEY_D, [0x08] = KEY_E,
    [0x09] = KEY_F, [0x0a] = KEY_G, [0x0b] = KEY_H, [0x0c] = KEY_I, [0x0d] = KEY_J,
    [0x0e] = KEY_K, [0x0f] = KEY_L, [0x10] = KEY_M, [0x11] = KEY_N, [0x12] = KEY_O,
    [0x13] = KEY_P, [0x14] = KEY_Q, [0x15] = KEY_R, [0x16] = KEY_S, [0x17] = KEY_T,
    [0x18] = KEY_U, [0x19] = KEY_V, [0x1a] = KEY_W, [0x1b] = KEY_X, [0x1c] = KEY_Y,
    [0x1d] = KEY_Z,
    [0x1e] = KEY_1, [0x1f] = KEY_2, [0x20] = KEY_3, [0x21] = KEY_4, [0x22] = KEY_5,
    [0x23] = KEY_6, [0x24] = KEY_7, [0x25] = KEY_8, [0x26] = KEY_9, [0x27] = KEY_0,
    [0x28] = KEY_ENTER, [0x29] = KEY_ESC, [0x2a] = KEY_BACKSPACE, [0x2b] = KEY_TAB,
    [0x2c] = KEY_SPACE, [0x2d] = KEY_MINUS, [0x2e] = KEY_EQUAL, [0x2f] = KEY_LEFTBRACE,
    [0x30] = KEY_RIGHTBRACE, [0x31] = KEY_BACKSLASH, [0x32] = KEY_BACKSLASH,
    [0x33] = KEY_SEMICOLON, [0x34] = KEY_APOSTROPHE, [0x35] = KEY_GRAVE, [0x36] = KEY_COMMA,
    [0x37] = KEY_DOT, [0x38] = KEY_SLASH, [0x39] = KEY_CAPSLOCK,
    [0x3a] = KEY_F1, [0x3b] = KEY_F2, [0x3c] = KEY_F3, [0x3d] = KEY_F4, [0x3e] = KEY_F5,
    [0x3f] = KEY_F6, [0x40] = KEY_F7, [0x41] = KEY_F8, [0x42] = KEY_F9, [0x43] = KEY_F10,
    [0x44] = KEY_F11, [0x45] = KEY_F12,
    [0x46] = KEY_SYSRQ, [0x47] = KEY_SCROLLLOCK, [0x48] = KEY_PAUSE, [0x49] = KEY_INSERT,
    [0x4a] = KEY_HOME, [0x4b] = KEY_PAGEUP, [0x4c] = KEY_DELETE, [0x4d] = KEY_END,
    [0x4e] = KEY_PAGEDOWN, [0x4f] = KEY_RIGHT, [0x50] = KEY_LEFT, [0x51] = KEY_DOWN,
    [0x52] = KEY_UP,
    [0x53] = KEY_NUMLOCK, [0x54] = KEY_KPSLASH, [0x55] = KEY_KPASTERISK, [0x56] = KEY_KPMINUS,
    [0x57] = KEY_KPPLUS, [0x58] = KEY_KPENTER, [0x59] = KEY_KP1, [0x5a] = KEY_KP2,
    [0x5b] = KEY_KP3, [0x5c] = KEY_KP4, [0x5d] = KEY_KP5, [0x5e] = KEY_KP6, [0x5f] = KEY_KP7,
    [0x60] = KEY_KP8, [0x61] = KEY_KP9, [0x62] = KEY_KP0, [0x63] = KEY_KPDOT,
    [0x64] = KEY_102ND, [0x65] = KEY_COMPOSE, [0x66] = KEY_POWER, [0x67] = KEY_KPEQUAL,
    [0x7f] = KEY_MUTE, [0x80] = KEY_VOLUMEUP, [0x81] = KEY_VOLUMEDOWN,
    [0x87] = KEY_RO, [0x88] = KEY_KATAKANAHIRAGANA, [0x89] = KEY_YEN, [0x8a] = KEY_HENKAN,
    [0x8b] = KEY_MUHENKAN, [0x90] = KEY_HANGEUL, [0x91] = KEY_HANJA, [0x94] = KEY_ZENKAKUHANKAKU,
    [0xe0] = KEY_LEFTCTRL, [0xe1] = KEY_LEFTSHIFT, [0xe2] = KEY_LEFTALT, [0xe3] = KEY_LEFTMETA,
    [0xe4] = KEY_RIGHTCTRL, [0xe5] = KEY_RIGHTSHIFT, [0xe6] = KEY_RIGHTALT, [0xe7] = KEY_RIGHTMETA,
};

/* The key code of a usage, or 0. */
static uint16_t usage_key(uint16_t page, uint16_t usage)
{
    switch (page) {
    case PAGE_KEYBOARD:
        return usage < sizeof keyboard_usage / sizeof keyboard_usage[0] ? keyboard_usage[usage] : 0;
    case PAGE_BUTTON:
        return usage >= 1 && usage <= 8 ? (uint16_t)(BTN_LEFT + usage - 1) : 0;
    case PAGE_DESKTOP:
        switch (usage) {
        case 0x81: return KEY_POWER;            /* system power down */
        case 0x82: return KEY_SLEEP;
        case 0x83: return KEY_WAKEUP;
        default:   return 0;
        }
    case PAGE_CONSUMER:
        switch (usage) {
        case 0xb5: return KEY_NEXTSONG;
        case 0xb6: return KEY_PREVIOUSSONG;
        case 0xb7: return KEY_STOPCD;
        case 0xcd: return KEY_PLAYPAUSE;
        case 0xe2: return KEY_MUTE;
        case 0xe9: return KEY_VOLUMEUP;
        case 0xea: return KEY_VOLUMEDOWN;
        default:   return 0;
        }
    default:
        return 0;
    }
}

/* The event of a variable field: a key, or an axis of the desktop page, or
 * the horizontal wheel of the consumer page (AC Pan). */
static void map_variable(struct hid_field *f)
{
    uint16_t key = usage_key(f->page, f->usage);
    if (key) {
        f->type = EV_KEY;
        f->code = key;
        return;
    }
    if (f->page == PAGE_DESKTOP) {
        switch (f->usage) {
        case 0x30:
            f->type = f->relative ? EV_REL : EV_ABS;
            f->code = f->relative ? REL_X : ABS_X;
            return;
        case 0x31:
            f->type = f->relative ? EV_REL : EV_ABS;
            f->code = f->relative ? REL_Y : ABS_Y;
            return;
        case 0x38:
            if (f->relative) {
                f->type = EV_REL;
                f->code = REL_WHEEL;
            }
            return;
        default:
            return;
        }
    }
    if (f->page == PAGE_CONSUMER && f->usage == 0x238 && f->relative) {
        f->type = EV_REL;
        f->code = REL_HWHEEL;
    }
}

static struct hid_report *report_of(struct hid_dev *h, uint8_t id)
{
    for (unsigned i = 0; i < h->nreports; i++)
        if (h->reports[i].id == id)
            return &h->reports[i];
    if (h->nreports == MAX_REPORTS)
        return NULL;
    struct hid_report *r = &h->reports[h->nreports++];
    r->id = id;
    return r;
}

static void mark_key(struct hid_dev *h, uint8_t id, uint16_t code)
{
    struct hid_report *r = report_of(h, id);
    if (r && code <= KEY_MAX)
        r->mask[code / 8] |= (uint8_t)(1u << (code % 8));
    input_set_key_cap(&h->input, code);
}

/* Record a field and declare its capabilities. */
static void add_field(struct hid_dev *h, const struct hid_field *f)
{
    if (h->nfields == MAX_FIELDS)
        return;
    if (f->array) {
        bool useful = false;
        for (int32_t v = f->logical_min; v <= f->logical_max && v - f->logical_min < 0x400; v++) {
            uint16_t key = usage_key(f->page, (uint16_t)(f->usage + (v - f->logical_min)));
            if (key) {
                mark_key(h, f->report_id, key);
                useful = true;
            }
        }
        if (useful)
            h->fields[h->nfields++] = *f;
        return;
    }
    struct hid_field v = *f;
    map_variable(&v);
    switch (v.type) {
    case EV_KEY:
        mark_key(h, v.report_id, v.code);
        break;
    case EV_REL:
        input_set_rel_cap(&h->input, v.code);
        break;
    case EV_ABS:
        input_set_abs_cap(&h->input, v.code, v.logical_min, v.logical_max, 0);
        break;
    default:
        return;
    }
    h->fields[h->nfields++] = v;
}

/* The state of the report descriptor parser: the global items with a
 * stack for Push and Pop, the local items of the next main item and the
 * bit position of each report ID. */
struct globals {
    uint16_t page;
    int32_t logical_min, logical_max;
    uint8_t report_size, report_count, report_id;
};

struct parser {
    struct globals g;
    struct globals stack[4];
    unsigned depth;
    uint32_t usages[16];
    unsigned nusages;
    uint32_t usage_min, usage_max;
    bool have_min, have_max;
    uint8_t ids[MAX_REPORTS];
    uint16_t offsets[MAX_REPORTS];
    unsigned nids;
};

static uint16_t *bit_offset(struct parser *p, uint8_t id)
{
    for (unsigned i = 0; i < p->nids; i++)
        if (p->ids[i] == id)
            return &p->offsets[i];
    if (p->nids == MAX_REPORTS)
        return NULL;
    p->ids[p->nids] = id;
    p->offsets[p->nids] = 0;
    return &p->offsets[p->nids++];
}

/* A usage of a local item: 4 data bytes carry the page in the upper 16
 * bits, fewer bytes are a usage on the current page. */
static uint32_t full_usage(const struct parser *p, uint32_t data, unsigned size)
{
    return size == 4 ? data : (uint32_t)p->g.page << 16 | (data & 0xffff);
}

static void input_item(struct hid_dev *h, struct parser *p, uint32_t flags)
{
    uint16_t *pos = bit_offset(p, p->g.report_id);
    if (!pos)
        return;
    unsigned size = p->g.report_size, count = p->g.report_count;
    bool constant = flags & 1, variable = flags & 2, relative = flags & 4;
    if (!constant && size >= 1 && size <= 32) {
        struct hid_field f = {
            .report_id = p->g.report_id,
            .relative = relative,
            .is_signed = p->g.logical_min < 0,
            .bit_size = (uint8_t)size,
            .logical_min = p->g.logical_min,
            .logical_max = p->g.logical_max,
        };
        if (variable) {
            f.count = 1;
            for (unsigned i = 0; i < count; i++) {
                uint32_t u;
                if (i < p->nusages)
                    u = p->usages[i];
                else if (p->have_min && p->usage_min + (i - p->nusages) <= p->usage_max)
                    u = p->usage_min + (i - p->nusages);
                else if (p->nusages)
                    u = p->usages[p->nusages - 1];
                else
                    continue;
                f.page = (uint16_t)(u >> 16);
                f.usage = (uint16_t)u;
                f.bit_offset = (uint16_t)(*pos + i * size);
                add_field(h, &f);
            }
        } else if (count <= 255) {
            uint32_t base = p->have_min ? p->usage_min : p->nusages ? p->usages[0] : 0;
            f.array = true;
            f.count = (uint8_t)count;
            f.page = (uint16_t)(base >> 16);
            f.usage = (uint16_t)base;
            f.bit_offset = *pos;
            add_field(h, &f);
        }
    }
    *pos = (uint16_t)(*pos + size * count);
}

static int parse_report_descriptor(struct hid_dev *h, const uint8_t *d, unsigned len)
{
    struct parser p;
    memset(&p, 0, sizeof p);
    unsigned i = 0;
    while (i < len) {
        uint8_t prefix = d[i++];
        if (prefix == 0xfe) {                   /* a long item, skipped */
            if (i + 1 >= len)
                break;
            i += 2u + d[i];
            continue;
        }
        unsigned size = prefix & 3;
        if (size == 3)
            size = 4;
        if (i + size > len)
            break;
        uint32_t data = 0;
        for (unsigned b = 0; b < size; b++)
            data |= (uint32_t)d[i + b] << (8 * b);
        int32_t sdata = size == 1 ? (int8_t)data : size == 2 ? (int16_t)data : (int32_t)data;
        i += size;
        unsigned type = (prefix >> 2) & 3, tag = prefix >> 4;
        if (type == 0) {                        /* main */
            if (tag == 0x8)
                input_item(h, &p, data);
            /* Output, Feature, Collection and End Collection carry no
             * input. Every main item ends the local items. */
            p.nusages = 0;
            p.have_min = p.have_max = false;
        } else if (type == 1) {                 /* global */
            switch (tag) {
            case 0x0: p.g.page = (uint16_t)data; break;
            case 0x1: p.g.logical_min = sdata; break;
            case 0x2:
                /* A maximum that reads as negative above a non-negative
                 * minimum was written without its sign byte. */
                p.g.logical_max = sdata;
                if (p.g.logical_min >= 0 && sdata < 0)
                    p.g.logical_max = (int32_t)data;
                break;
            case 0x7: p.g.report_size = (uint8_t)data; break;
            case 0x8: p.g.report_id = (uint8_t)data; h->uses_ids = true; break;
            case 0x9: p.g.report_count = (uint8_t)MIN(data, 255u); break;
            case 0xa:
                if (p.depth < 4)
                    p.stack[p.depth++] = p.g;
                break;
            case 0xb:
                if (p.depth > 0)
                    p.g = p.stack[--p.depth];
                break;
            default: break;
            }
        } else if (type == 2) {                 /* local */
            switch (tag) {
            case 0x0:
                if (p.nusages < 16)
                    p.usages[p.nusages++] = full_usage(&p, data, size);
                break;
            case 0x1: p.usage_min = full_usage(&p, data, size); p.have_min = true; break;
            case 0x2: p.usage_max = full_usage(&p, data, size); p.have_max = true; break;
            default: break;
            }
        }
    }
    unsigned bits = 0;
    for (unsigned k = 0; k < p.nids; k++)
        bits = MAX(bits, (unsigned)p.offsets[k]);
    h->report_bytes = (bits + 7) / 8 + (h->uses_ids ? 1 : 0);
    return h->nfields ? 0 : -ENODEV;
}

/* The boot keyboard report: the modifier bits as eight variables of the
 * usages 0xe0 to 0xe7, a reserved byte and six key slots of an array. */
static void boot_keyboard_fields(struct hid_dev *h)
{
    struct hid_field f = { .page = PAGE_KEYBOARD, .bit_size = 1, .count = 1, .logical_max = 1 };
    for (unsigned i = 0; i < 8; i++) {
        f.usage = (uint16_t)(0xe0 + i);
        f.bit_offset = (uint16_t)i;
        add_field(h, &f);
    }
    struct hid_field keys = {
        .page = PAGE_KEYBOARD, .array = true, .bit_size = 8, .count = 6,
        .bit_offset = 16, .usage = 0, .logical_min = 0, .logical_max = 0xff,
    };
    add_field(h, &keys);
}

static uint32_t get_bits(const uint8_t *data, unsigned len, unsigned offset, unsigned size)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) {
        unsigned bit = offset + i;
        if (bit / 8 >= len)
            break;
        if (data[bit / 8] & (1u << (bit % 8)))
            v |= 1u << i;
    }
    return v;
}

static int32_t field_value(const struct hid_field *f, const uint8_t *data, unsigned len, unsigned slot)
{
    uint32_t v = get_bits(data, len, f->bit_offset + slot * f->bit_size, f->bit_size);
    if (f->is_signed && f->bit_size < 32 && (v & (1u << (f->bit_size - 1))))
        v |= ~0u << f->bit_size;
    return (int32_t)v;
}

/* One report from the interrupt endpoint, under xhci.lock. */
static void hid_report(void *arg, const uint8_t *data, unsigned len)
{
    struct hid_dev *h = arg;
    uint8_t id = 0;
    if (h->uses_ids) {
        if (len == 0)
            return;
        id = data[0];
        data++;
        len--;
    }
    struct hid_report *r = NULL;
    for (unsigned i = 0; i < h->nreports; i++)
        if (h->reports[i].id == id)
            r = &h->reports[i];
    uint8_t down[KEY_BITS_BYTES];
    memset(down, 0, sizeof down);
    bool keys_valid = true;
    for (unsigned i = 0; i < h->nfields; i++) {
        const struct hid_field *f = &h->fields[i];
        if (f->report_id != id)
            continue;
        if (f->array) {
            for (unsigned s = 0; s < f->count; s++) {
                int32_t v = field_value(f, data, len, s);
                if (v < f->logical_min || v > f->logical_max)
                    continue;
                uint16_t usage = (uint16_t)(f->usage + (v - f->logical_min));
                /* ErrorRollOver, POSTFail and ErrorUndefined: the report
                 * does not say which keys are down. */
                if (f->page == PAGE_KEYBOARD && usage >= 1 && usage <= 3)
                    keys_valid = false;
                uint16_t key = usage_key(f->page, usage);
                if (key)
                    down[key / 8] |= (uint8_t)(1u << (key % 8));
            }
            continue;
        }
        int32_t v = field_value(f, data, len, 0);
        switch (f->type) {
        case EV_KEY:
            if (v)
                down[f->code / 8] |= (uint8_t)(1u << (f->code % 8));
            break;
        case EV_REL:
            if (v)
                input_report_rel(&h->input, f->code, v);
            break;
        case EV_ABS:
            input_report_abs(&h->input, f->code, v);
            break;
        default:
            break;
        }
    }
    if (r && keys_valid) {
        for (unsigned b = 0; b < KEY_BITS_BYTES; b++) {
            uint8_t changed = (uint8_t)((down[b] ^ h->keys[b]) & r->mask[b]);
            for (unsigned bit = 0; changed && bit < 8; bit++) {
                if (!(changed & (1u << bit)))
                    continue;
                changed &= (uint8_t)~(1u << bit);
                input_report_key(&h->input, (uint16_t)(b * 8 + bit), down[b] & (1u << bit));
            }
            h->keys[b] = (uint8_t)((h->keys[b] & ~r->mask[b]) | (down[b] & r->mask[b]));
        }
    }
    input_sync(&h->input);
}

/* The interrupt IN endpoint and the HID descriptor of the interface that
 * starts at intf. */
static void find_descriptors(const uint8_t *cfg, unsigned len, const struct usb_interface_descriptor *intf,
                             const struct usb_endpoint_descriptor **ep, const struct usb_hid_descriptor **hid)
{
    unsigned off = (unsigned)((const uint8_t *)intf - cfg) + intf->bLength;
    *ep = NULL;
    *hid = NULL;
    for (; off + 2 <= len && cfg[off] >= 2; off += cfg[off]) {
        uint8_t type = cfg[off + 1];
        if (type == USB_DT_INTERFACE)
            break;
        if (type == USB_DT_HID && cfg[off] >= sizeof(struct usb_hid_descriptor) && !*hid)
            *hid = (const void *)(cfg + off);
        if (type == USB_DT_ENDPOINT && cfg[off] >= sizeof(struct usb_endpoint_descriptor) && !*ep) {
            const struct usb_endpoint_descriptor *e = (const void *)(cfg + off);
            if ((e->bEndpointAddress & USB_DIR_IN) && (e->bmAttributes & USB_EP_XFER_MASK) == USB_EP_XFER_INT)
                *ep = e;
        }
    }
}

static bool same_capabilities(const struct input_dev *a, const struct input_dev *b)
{
    return a->evbit == b->evbit && a->relbit == b->relbit && a->absbit == b->absbit &&
           memcmp(a->keybit, b->keybit, sizeof a->keybit) == 0;
}

/* An input device of a disconnected interface with the name and the
 * capabilities of h, taken from the free list, or NULL. */
static struct hid_dev *take_free(const struct hid_dev *h)
{
    struct hid_dev *found = NULL;
    spin_lock(&hid_free_lock);
    struct list_head *pos;
    list_for_each(pos, &hid_free) {
        struct hid_dev *f = container_of(pos, struct hid_dev, link);
        if (strcmp(f->input.name, h->input.name) == 0 && same_capabilities(&f->input, &h->input)) {
            list_del(&f->link);
            found = f;
            break;
        }
    }
    spin_unlock(&hid_free_lock);
    return found;
}

int hid_probe(struct usb_device *dev, const uint8_t *cfg, unsigned cfg_len,
              const struct usb_interface_descriptor *intf)
{
    unsigned slot = 0;
    while (slot < USB_MAX_HID && dev->hid[slot])
        slot++;
    if (slot == USB_MAX_HID)
        return -ENOSPC;
    const struct usb_endpoint_descriptor *ep;
    const struct usb_hid_descriptor *hdesc;
    find_descriptors(cfg, cfg_len, intf, &ep, &hdesc);
    if (!ep)
        return -ENODEV;
    struct hid_dev *h = kzalloc(sizeof *h);
    if (!h)
        return -ENOMEM;
    h->usb = dev;
    h->interface = intf->bInterfaceNumber;
    char name[INPUT_NAME_MAX];
    if (intf->bInterfaceNumber)
        ksnprintf(name, sizeof name, "%s (interface %u)", dev->product, intf->bInterfaceNumber);
    else
        strlcpy(name, dev->product, sizeof name);
    input_dev_init(&h->input, name, BUS_USB);
    h->input.id.vendor = dev->desc.idVendor;
    h->input.id.product = dev->desc.idProduct;
    h->input.id.version = dev->desc.bcdDevice;

    uint8_t rtype = USB_TYPE_CLASS | USB_RECIP_INTERFACE;
    bool boot_keyboard = intf->bInterfaceSubClass == HID_SUBCLASS_BOOT &&
                         intf->bInterfaceProtocol == HID_PROTOCOL_KEYBOARD;
    int r = -ENODEV;
    if (boot_keyboard) {
        xhci_control(dev, rtype, HID_REQ_SET_PROTOCOL, 0, intf->bInterfaceNumber, NULL, 0);
        xhci_control(dev, rtype, HID_REQ_SET_IDLE, 0, intf->bInterfaceNumber, NULL, 0);
        boot_keyboard_fields(h);
        h->boot = true;
        h->report_bytes = 8;
        r = 0;
    } else if (hdesc && hdesc->bReportDescriptorType == USB_DT_REPORT && hdesc->wReportDescriptorLength) {
        unsigned rlen = MIN((unsigned)hdesc->wReportDescriptorLength, 4096u);
        uint8_t *rd = kmalloc(rlen);
        if (!rd) {
            kfree(h);
            return -ENOMEM;
        }
        if (intf->bInterfaceSubClass == HID_SUBCLASS_BOOT)
            xhci_control(dev, rtype, HID_REQ_SET_PROTOCOL, 1, intf->bInterfaceNumber, NULL, 0);
        xhci_control(dev, rtype, HID_REQ_SET_IDLE, 0, intf->bInterfaceNumber, NULL, 0);
        r = xhci_control(dev, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_INTERFACE, USB_REQ_GET_DESCRIPTOR,
                         USB_DT_REPORT << 8, intf->bInterfaceNumber, rd, (uint16_t)rlen);
        if (r >= 0)
            r = parse_report_descriptor(h, rd, rlen);
        kfree(rd);
    }
    if (r < 0) {
        klog_info("%s: interface %u reports nothing the input core knows", dev->product,
                  intf->bInterfaceNumber);
        kfree(h);
        return r;
    }
    bool keyboard = h->input.keybit[KEY_A / 8] & (1u << (KEY_A % 8));
    if (keyboard)
        input_set_repeat(&h->input, 500, 33);

    /* A device that was plugged in before reuses its input device, which
     * the input core cannot remove. */
    struct hid_dev *old = take_free(h);
    if (old) {
        old->usb = dev;
        old->interface = h->interface;
        old->uses_ids = h->uses_ids;
        old->boot = h->boot;
        old->report_bytes = h->report_bytes;
        old->nfields = h->nfields;
        memcpy(old->fields, h->fields, sizeof h->fields);
        old->nreports = h->nreports;
        memcpy(old->reports, h->reports, sizeof h->reports);
        memset(old->keys, 0, sizeof old->keys);
        kfree(h);
        h = old;
    } else if (input_register_device(&h->input) < 0) {
        kfree(h);
        return -ENOMEM;
    }
    dev->hid[slot] = h;
    r = xhci_interrupt_in(dev, ep, h->report_bytes, hid_report, h);
    if (r < 0) {
        klog_warn("%s: the interrupt endpoint does not start: %d", h->input.name, r);
        dev->hid[slot] = NULL;
        h->usb = NULL;
        spin_lock(&hid_free_lock);
        list_add(&h->link, &hid_free);
        spin_unlock(&hid_free_lock);
        return r;
    }
    const char *kind = keyboard ? "keyboard"
                     : (h->input.absbit & (1u << ABS_X)) ? "absolute pointer"
                     : (h->input.relbit & (1u << REL_X)) ? "relative pointer" : "keys";
    klog_info("%s: %s, /dev/input/event%u%s", h->input.name, kind, h->input.index,
              old ? ", reused" : "");
    return 0;
}

void hid_disconnect(struct hid_dev *h)
{
    /* Release the keys that are down, so that no key remains pressed in the
     * input core. */
    for (unsigned b = 0; b < KEY_BITS_BYTES; b++)
        for (unsigned bit = 0; h->keys[b] && bit < 8; bit++)
            if (h->keys[b] & (1u << bit)) {
                input_report_key(&h->input, (uint16_t)(b * 8 + bit), 0);
                h->keys[b] &= (uint8_t)~(1u << bit);
            }
    input_sync(&h->input);
    h->usb = NULL;
    spin_lock(&hid_free_lock);
    list_add(&h->link, &hid_free);
    spin_unlock(&hid_free_lock);
}

unsigned hid_interface(const struct hid_dev *h)
{
    return h->interface;
}

/* The fields are written by hid_probe before the device is published to
 * /dev/devices and are read only afterwards. */
void hid_describe(struct devinfo *d, const struct hid_dev *h)
{
    devinfo_prop(d, "input_device", "/dev/input/event%u (%s)", h->input.index, h->input.name);
    devinfo_prop(d, "report_protocol", "%s", h->boot ? "boot" : "report");
    devinfo_prop(d, "report_ids", "%s", h->uses_ids ? "yes" : "no");
    devinfo_prop(d, "input_fields", "%u", h->nfields);
    devinfo_prop(d, "report_length", "%u bytes", h->report_bytes);
}
