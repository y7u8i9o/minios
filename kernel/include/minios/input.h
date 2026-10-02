#pragma once
/* The input event interface shared by the kernel and user space.
 *
 * Every input device is a node /dev/input/eventN delivering fixed size
 * records of struct input_event in the evdev style: a type, a code and a
 * value, terminated by an EV_SYN/SYN_REPORT record that ends one report
 * of the device. Key codes are the Linux key codes, which the virtio
 * keyboard delivers unchanged and the PS/2 driver translates to. */
#include <stdint.h>

struct input_event {
    uint64_t time_us;       /* monotonic microseconds (CLOCK_MONOTONIC) */
    uint16_t type;          /* EV_* */
    uint16_t code;          /* KEY_*, BTN_*, REL_*, ABS_* */
    int32_t value;          /* key: 1 press, 0 release, 2 repeat; rel: delta; abs: position */
};

/* Event types. */
#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_REL 0x02
#define EV_ABS 0x03
#define EV_REP 0x14
#define EV_CNT 0x20

/* EV_SYN codes. SYN_DROPPED is queued once when a reader's queue
 * overflowed: the reader lost events and must fetch the key state with
 * EVIOCGKEY before trusting its own copy again. */
#define SYN_REPORT  0
#define SYN_DROPPED 3

/* Key codes (the Linux numbering; 1..88 equal the PS/2 set 1 make codes). */
#define KEY_RESERVED   0
#define KEY_ESC        1
#define KEY_1          2
#define KEY_2          3
#define KEY_3          4
#define KEY_4          5
#define KEY_5          6
#define KEY_6          7
#define KEY_7          8
#define KEY_8          9
#define KEY_9          10
#define KEY_0          11
#define KEY_MINUS      12
#define KEY_EQUAL      13
#define KEY_BACKSPACE  14
#define KEY_TAB        15
#define KEY_Q          16
#define KEY_W          17
#define KEY_E          18
#define KEY_R          19
#define KEY_T          20
#define KEY_Y          21
#define KEY_U          22
#define KEY_I          23
#define KEY_O          24
#define KEY_P          25
#define KEY_LEFTBRACE  26
#define KEY_RIGHTBRACE 27
#define KEY_ENTER      28
#define KEY_LEFTCTRL   29
#define KEY_A          30
#define KEY_S          31
#define KEY_D          32
#define KEY_F          33
#define KEY_G          34
#define KEY_H          35
#define KEY_J          36
#define KEY_K          37
#define KEY_L          38
#define KEY_SEMICOLON  39
#define KEY_APOSTROPHE 40
#define KEY_GRAVE      41
#define KEY_LEFTSHIFT  42
#define KEY_BACKSLASH  43
#define KEY_Z          44
#define KEY_X          45
#define KEY_C          46
#define KEY_V          47
#define KEY_B          48
#define KEY_N          49
#define KEY_M          50
#define KEY_COMMA      51
#define KEY_DOT        52
#define KEY_SLASH      53
#define KEY_RIGHTSHIFT 54
#define KEY_KPASTERISK 55
#define KEY_LEFTALT    56
#define KEY_SPACE      57
#define KEY_CAPSLOCK   58
#define KEY_F1         59
#define KEY_F2         60
#define KEY_F3         61
#define KEY_F4         62
#define KEY_F5         63
#define KEY_F6         64
#define KEY_F7         65
#define KEY_F8         66
#define KEY_F9         67
#define KEY_F10        68
#define KEY_NUMLOCK    69
#define KEY_SCROLLLOCK 70
#define KEY_KP7        71
#define KEY_KP8        72
#define KEY_KP9        73
#define KEY_KPMINUS    74
#define KEY_KP4        75
#define KEY_KP5        76
#define KEY_KP6        77
#define KEY_KPPLUS     78
#define KEY_KP1        79
#define KEY_KP2        80
#define KEY_KP3        81
#define KEY_KP0        82
#define KEY_KPDOT      83
#define KEY_ZENKAKUHANKAKU 85
#define KEY_102ND      86
#define KEY_F11        87
#define KEY_F12        88
#define KEY_RO         89
#define KEY_HENKAN     92
#define KEY_KATAKANAHIRAGANA 93
#define KEY_MUHENKAN   94
#define KEY_KPENTER    96
#define KEY_RIGHTCTRL  97
#define KEY_KPSLASH    98
#define KEY_SYSRQ      99
#define KEY_RIGHTALT   100
#define KEY_HOME       102
#define KEY_UP         103
#define KEY_PAGEUP     104
#define KEY_LEFT       105
#define KEY_RIGHT      106
#define KEY_END        107
#define KEY_DOWN       108
#define KEY_PAGEDOWN   109
#define KEY_INSERT     110
#define KEY_DELETE     111
#define KEY_MUTE       113
#define KEY_VOLUMEDOWN 114
#define KEY_VOLUMEUP   115
#define KEY_POWER      116
#define KEY_KPEQUAL    117
#define KEY_PAUSE      119
#define KEY_HANGEUL    122         /* LANG1: Hangul, the Kana key of Mac keyboards */
#define KEY_HANJA      123         /* LANG2: Hanja, the Eisu key of Mac keyboards */
#define KEY_YEN        124
#define KEY_LEFTMETA   125
#define KEY_RIGHTMETA  126
#define KEY_COMPOSE    127
#define KEY_SLEEP      142
#define KEY_WAKEUP     143
#define KEY_NEXTSONG   163
#define KEY_PLAYPAUSE  164
#define KEY_PREVIOUSSONG 165
#define KEY_STOPCD     166

#define BTN_MOUSE      0x110
#define BTN_LEFT       0x110
#define BTN_RIGHT      0x111
#define BTN_MIDDLE     0x112
#define BTN_SIDE       0x113
#define BTN_EXTRA      0x114
#define BTN_FORWARD    0x115
#define BTN_BACK       0x116
#define BTN_TASK       0x117

#define KEY_MAX        0x2ff
#define KEY_CNT        (KEY_MAX + 1)
#define INPUT_KEY_BYTES ((KEY_CNT + 7) / 8)

/* Relative axes. */
#define REL_X          0x00
#define REL_Y          0x01
#define REL_HWHEEL     0x06
#define REL_WHEEL      0x08
#define REL_WHEEL_HI_RES  0x0b
#define REL_HWHEEL_HI_RES 0x0c
#define REL_MAX        0x0f
#define REL_CNT        (REL_MAX + 1)

/* Absolute axes. */
#define ABS_X          0x00
#define ABS_Y          0x01
#define ABS_MAX        0x07
#define ABS_CNT        (ABS_MAX + 1)

/* EV_REP codes and the value array of EVIOCGREP / EVIOCSREP. */
#define REP_DELAY      0
#define REP_PERIOD     1
#define REP_CNT        2

/* Bus types of struct input_id. */
#define BUS_VIRTUAL    0x06
#define BUS_I8042      0x11
#define BUS_VIRTIO     0x1c

struct input_id {
    uint16_t bustype;
    uint16_t vendor;
    uint16_t product;
    uint16_t version;
};

struct input_absinfo {
    int32_t value;
    int32_t minimum;
    int32_t maximum;
    int32_t fuzz;
    int32_t flat;
    int32_t resolution;
};

/* Capabilities of a device, filled by EVIOCGCAPS. A device reports
 * EV_REP when the kernel repeats its keys. */
#define INPUT_NAME_MAX 64
struct input_caps {
    uint32_t ev_bits;                   /* bit n set: EV_n is reported */
    uint32_t rel_bits;                  /* bit n set: REL_n is reported */
    uint32_t abs_bits;                  /* bit n set: ABS_n is reported */
    uint8_t key_bits[INPUT_KEY_BYTES];  /* bit n set: KEY_n or BTN_n exists */
};

/* ioctl requests of /dev/input/eventN. */
#define EVIOCGVERSION 0x4501    /* uint32_t *: the interface version, 1 */
#define EVIOCGID      0x4502    /* struct input_id * */
#define EVIOCGNAME    0x4503    /* char[INPUT_NAME_MAX] */
#define EVIOCGCAPS    0x4504    /* struct input_caps * */
#define EVIOCGKEY     0x4505    /* uint8_t[INPUT_KEY_BYTES]: keys currently down */
#define EVIOCGREP     0x4506    /* uint32_t[REP_CNT]: delay and period, ms */
#define EVIOCSREP     0x4507    /* const uint32_t[REP_CNT] */
#define EVIOCGRAB     0x4508    /* value 1 grabs the device for this descriptor, 0 releases */
#define EVIOCGABS(a)  (0x4540 + (a))   /* struct input_absinfo * for axis a */
#define EV_VERSION    1
