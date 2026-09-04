#pragma once
#include <kernel.h>
#include <sync/spinlock.h>
#include <sched/wait.h>
#include <minios/abi.h>

#define TTY_LINE_MAX  256
#define TTY_READY_MAX 1024

struct tty;
typedef void (*tty_output_fn)(struct tty *t, const char *s, size_t n);

/* A terminal: the line discipline between an input source (keyboard or
 * pseudo terminal master) and reading processes. lock protects every
 * field below and is the condition lock of rd_waitq; it may be taken in
 * interrupt context, so nothing under it blocks. */
struct tty {
    const char *name;
    struct spinlock lock;
    struct waitq rd_waitq;
    uint32_t lflag;                 /* ICANON, ECHO, ISIG, KBD_SCANCODES */
    int fg_pgid;
    uint16_t cols, rows;
    char line[TTY_LINE_MAX];
    size_t line_len;
    char ready[TTY_READY_MAX];      /* bytes for readers */
    size_t head, tail, count;
    bool hangup;                    /* readers get end of file */
    int signal_pending;             /* control key seen, delivered by ttyd */
    bool defer_signals;             /* input arrives in interrupt context */
    tty_output_fn output;           /* echo and control sequences */
};

void tty_init(struct tty *t, const char *name, tty_output_fn output, bool defer_signals);
/* Feed one translated character through the line discipline. Safe in
 * interrupt context when defer_signals is set. */
void tty_input_char(struct tty *t, char c);
/* Deliver bytes to readers unchanged (escape sequences, raw scancodes). */
void tty_input_raw(struct tty *t, const char *s, size_t n);
/* Blocking read for a process; returns -EINTR on a signal, 0 at hangup. */
long tty_read(struct tty *t, char *buf, size_t n);
int tty_poll(struct tty *t);
long tty_ioctl(struct tty *t, unsigned long req, uintptr_t arg);
uint32_t tty_get_lflag(struct tty *t);
void tty_set_lflag(struct tty *t, uint32_t lflag);
int tty_get_fg_pgid(struct tty *t);
void tty_set_fg_pgid(struct tty *t, int pgid);
void tty_hangup(struct tty *t);
/* Non blocking single byte read, for kernel tests. -1 if none. */
int tty_getc(struct tty *t);
size_t tty_available(struct tty *t);

/* The console terminal, fed by the keyboard driver, output to the console. */
extern struct tty console_tty;
/* Thread turning console control keys into signals. Needs the scheduler. */
void tty_start_daemon(void);
void console_tty_init(void);
