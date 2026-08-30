#pragma once
#include <kernel.h>

#define KBD_BUF_SIZE 256

void ps2kbd_init(void);
/* Translate one scancode (set 1) and feed the result to the line buffer.
 * Called by the interrupt handler and by tests. */
void ps2kbd_feed_scancode(uint8_t code);
/* Next character of a completed line, or -1 if no complete line is ready. */
int ps2kbd_getc(void);
/* Number of characters in completed lines waiting to be read. */
size_t ps2kbd_available(void);
/* Blocking read for user space: waits for a completed line and returns up
 * to n characters of it. Returns -EINTR if the process is exiting. */
long ps2kbd_read(char *buf, size_t n);
/* Foreground process group receiving SIGINT on control C. */
void ps2kbd_set_fg_pgid(int pgid);
int ps2kbd_get_fg_pgid(void);
/* Start the thread that turns control C into SIGINT. Needs the scheduler. */
void ps2kbd_start_ttyd(void);
/* Line discipline flags (ICANON, ECHO, ISIG from minios/abi.h). */
uint32_t ps2kbd_get_lflag(void);
/* Readiness for poll. */
int ps2kbd_poll(void);
void ps2kbd_set_lflag(uint32_t lflag);
