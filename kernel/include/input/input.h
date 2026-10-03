#pragma once
/* The input core: drivers register devices and report events; the core
 * retains the key state of every device, repeats pressed keys, delivers the
 * events to readers of /dev/input/eventN and feeds keyboards that no
 * reader has grabbed to the console terminal (input/keyboard.c).
 * docs/design/input.md describes the design. */
#include <kernel.h>
#include <minios/input.h>
#include <sync/spinlock.h>
#include <lib/list.h>

struct input_reader;

/* A device. The driver fills name, id, the capability bits and the axis
 * ranges with the input_set_* helpers before input_register_device; the
 * core owns the remaining fields.
 *
 * lock protects key, rep, the repeat fields, pending, readers and grab,
 * and every reader's queue. It is taken from interrupt handlers and from
 * virtqueue completion callbacks (under a virtqueue lock). Order:
 * input_devices_lock -> input_dev.lock -> waitq.lock / poll_source.lock;
 * the console keyboard state (kbd_lock) and console_tty.lock are taken
 * only after input_dev.lock has been released. */
struct input_dev {
    char name[INPUT_NAME_MAX];
    struct input_id id;
    uint32_t evbit, relbit, absbit;
    uint8_t keybit[INPUT_KEY_BYTES];
    struct input_absinfo abs[ABS_CNT];

    struct spinlock lock;
    uint8_t key[INPUT_KEY_BYTES];       /* keys currently down */
    uint32_t rep[REP_CNT];              /* repeat delay and period in ms */
    uint16_t repeat_key;
    bool repeat_active;
    uint64_t repeat_at_ms;              /* timer_ms deadline of the next repeat */
    bool pending;                       /* an event since the last SYN_REPORT */
    unsigned index;                     /* N of /dev/input/eventN */
    struct list_head readers;           /* struct input_reader.link */
    struct input_reader *grab;          /* the reader with EVIOCGRAB active */
    struct list_head link;              /* input_devices, under input_devices_lock */
};

/* Prepare a device: name, bus type, no capabilities. */
void input_dev_init(struct input_dev *dev, const char *name, uint16_t bustype);

static inline void input_set_key_cap(struct input_dev *dev, unsigned code)
{
    if (code <= KEY_MAX) {
        dev->evbit |= 1u << EV_KEY;
        dev->keybit[code / 8] |= (uint8_t)(1u << (code % 8));
    }
}

static inline void input_set_rel_cap(struct input_dev *dev, unsigned axis)
{
    if (axis <= REL_MAX) {
        dev->evbit |= 1u << EV_REL;
        dev->relbit |= 1u << axis;
    }
}

void input_set_abs_cap(struct input_dev *dev, unsigned axis, int32_t minimum, int32_t maximum, int32_t resolution);
/* Enable software key repeat with the given timing (EV_REP is reported). */
void input_set_repeat(struct input_dev *dev, uint32_t delay_ms, uint32_t period_ms);

/* Register the device and create /dev/input/eventN. */
int input_register_device(struct input_dev *dev);

/* Report one event. Callable from interrupt handlers and virtqueue
 * completions. Presses of keys already down and releases of keys already
 * up are dropped; the core generates the repeats (value 2) itself. */
void input_event(struct input_dev *dev, uint16_t type, uint16_t code, int32_t value);

static inline void input_report_key(struct input_dev *dev, uint16_t code, int down)
{
    input_event(dev, EV_KEY, code, down ? 1 : 0);
}

static inline void input_report_rel(struct input_dev *dev, uint16_t axis, int32_t delta)
{
    input_event(dev, EV_REL, axis, delta);
}

static inline void input_report_abs(struct input_dev *dev, uint16_t axis, int32_t value)
{
    input_event(dev, EV_ABS, axis, value);
}

static inline void input_sync(struct input_dev *dev)
{
    input_event(dev, EV_SYN, SYN_REPORT, 0);
}

/* Whether a reader has grabbed the device (its keys then bypass the console). */
bool input_dev_grabbed(struct input_dev *dev);
/* Whether the key is down on the device. */
bool input_key_down(struct input_dev *dev, unsigned code);
/* The device with this name, or NULL (tests). */
struct input_dev *input_device_by_name(const char *name);

/* Create /dev/input; before any device registers. */
void input_init(void);
/* Start the key repeat thread. Needs the scheduler. */
void input_start_daemon(void);

/* input/keyboard.c: keys of ungrabbed keyboards, translated for the
 * console terminal. value is 1 press, 0 release, 2 repeat. */
void input_console_key(uint16_t code, int value);
/* Replace the layout of the console keyboard (KBD_SET_KEYMAP).  Returns 0
 * or -EINVAL for a malformed keymap. */
struct kbd_keymap;
int input_console_set_keymap(const struct kbd_keymap *map);
