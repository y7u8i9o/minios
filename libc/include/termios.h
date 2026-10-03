#pragma once
#include <minios/abi.h>

/* Terminal attributes (struct termios and its flags in minios/abi.h). The
 * line discipline acts on ISIG, ICANON and ECHO, and preserves the rest as
 * set. TCSAFLUSH discards the unread input before the change, and the
 * speeds are recorded without effect. */
typedef uint32_t tcflag_t;
typedef uint8_t cc_t;
typedef uint32_t speed_t;

#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2

int tcgetattr(int fd, struct termios *t);
int tcsetattr(int fd, int action, const struct termios *t);
int tcflush(int fd, int queue);
int tcdrain(int fd);
int tcsendbreak(int fd, int duration);
speed_t cfgetispeed(const struct termios *t);
speed_t cfgetospeed(const struct termios *t);
int cfsetispeed(struct termios *t, speed_t speed);
int cfsetospeed(struct termios *t, speed_t speed);
int cfsetspeed(struct termios *t, speed_t speed);
/* Switch to raw mode, without line editing, echo, signals or translations. */
void cfmakeraw(struct termios *t);
/* Open a pseudo terminal pair as BSD does, returning the master and the
 * slave descriptors, the slave's path in name (32 bytes suffice) unless name
 * is NULL, and the attributes and window size of the slave when termp and
 * winp are not NULL. */
struct winsize;
int openpty(int *master, int *slave, char *name, const struct termios *termp, const struct winsize *winp);
