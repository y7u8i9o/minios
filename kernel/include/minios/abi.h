#pragma once
/* Binary interface shared between the kernel and libc: structures crossing
 * the system call boundary. Only fixed width types are used here. */
#include <stdint.h>

/* File type bits in st_mode. */
#define S_IFMT   0170000
#define S_IFDIR  0040000
#define S_IFREG  0100000
#define S_IFCHR  0020000
#define S_IFBLK  0060000
#define S_IFIFO  0010000
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISCHR(m) (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m) (((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)

struct stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize;
    int64_t  st_blocks;
    int64_t  st_mtime;
};

/* Directory entry types. */
#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8

#define NAME_MAX 255

/* getdents fills an array of these fixed size records. */
struct dirent {
    uint64_t d_ino;
    uint8_t  d_type;
    char     d_name[NAME_MAX + 1];
};

/* open flags */
#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_ACCMODE   3
#define O_CREAT     0x40
#define O_EXCL      0x80
#define O_TRUNC     0x200
#define O_APPEND    0x400
#define O_DIRECTORY 0x10000
#define O_NONBLOCK  0x800
#define O_CLOEXEC   0x80000

/* lseek whence */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* mmap */
#define PROT_NONE  0
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20
#define MAP_FAILED ((void *)-1)

/* Per process descriptor table size. */
#define OPEN_MAX 64

/* Signals. */
#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGILL    4
#define SIGABRT   6
#define SIGFPE    8
#define SIGKILL   9
#define SIGUSR1  10
#define SIGSEGV  11
#define SIGUSR2  12
#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15
#define SIGCHLD  17
#define SIGWINCH 28
#define NSIG     32

#define SIG_DFL ((void (*)(int))0)
#define SIG_IGN ((void (*)(int))1)
#define SIG_ERR ((void (*)(int))-1)

#define SA_NODEFER  0x40000000
#define SA_RESTORER 0x04000000

#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

typedef uint64_t sigset_t;

struct sigaction {
    void (*sa_handler)(int);
    sigset_t sa_mask;
    int sa_flags;
    void (*sa_restorer)(void);
};

/* reboot */
#define RB_POWER_OFF 0
#define RB_AUTOBOOT  1
#define RB_HALT      2

/* Terminal control: a small termios subset for the console. */
#define ICANON 0x0002   /* line editing, deliver whole lines */
#define ECHO   0x0008   /* echo typed characters */
#define ISIG   0x0001   /* control C sends SIGINT */

struct termios {
    uint32_t c_lflag;
};

#define TCGETS   0x5401
#define TCSETS   0x5402
#define TIOCGWINSZ 0x5413

struct winsize {
    uint16_t ws_row;
    uint16_t ws_col;
};

/* /dev/mouse delivers fixed size events. */
struct mouse_event {
    int16_t dx, dy;         /* movement, dy positive downwards */
    uint8_t buttons;        /* bit 0 left, 1 right, 2 middle */
    int8_t dz;              /* wheel, positive towards the user (down) */
    uint8_t pad[2];
    uint32_t time_ms;
};

/* Raw scancode mode for /dev/kbd and /dev/console: bytes from the
 * keyboard controller are delivered untranslated (press and release,
 * with the 0xe0 prefix), no echo, no line editing, no SIGINT. */
#define KBD_SCANCODES 0x100

/* uname */
#define UTS_LEN 32
struct utsname {
    char sysname[UTS_LEN];
    char nodename[UTS_LEN];
    char release[UTS_LEN];
    char version[UTS_LEN];
    char machine[UTS_LEN];
};

/* /dev/fb0 */
struct fb_info {
    uint32_t width, height;
    uint32_t pitch;         /* bytes per row */
    uint32_t bpp;           /* 24 or 32 */
    /* Channel layout: the value of a channel is (pixel >> shift) & ((1 << size) - 1). */
    uint8_t red_size, red_shift;
    uint8_t green_size, green_shift;
    uint8_t blue_size, blue_shift;
    uint8_t pad[2];
};
#define FBIOGET_INFO 0x4600
#define FBIO_ACQUIRE 0x4601  /* stop the kernel text console from drawing */
#define FBIO_RELEASE 0x4602  /* restore the text console */

#define MAP_SHARED 0x01

/* Message queues: fixed size messages. */
#define MQ_MSG_MAX   256
#define MQ_DEPTH     64
#define MQ_NAME_MAX  32
#define MQ_CREATE    0x1     /* mq_open flag: create if missing */
#define MQ_EXCL      0x2

/* poll: readiness bits and the descriptor array element. */
#define POLLIN  1
#define POLLERR 8
#define POLLHUP 0x10
#define POLLNVAL 0x20
#define POLLOUT 4
struct pollfd {
    int32_t fd;
    int16_t events;
    int16_t revents;
};

/* shm_open flags */
#define SHM_CREATE 0x1
#define SHM_EXCL   0x2
#define SHM_NAME_MAX 32

/* More terminal requests. */
#define TIOCSWINSZ 0x5414
#define TIOCGPGRP  0x540f
#define TIOCSPGRP  0x5410
#define TIOCGPTN   0x5430   /* pseudo terminal master: index of the slave */

/* ---- M23: sockets, descriptor passing, memfd, eventfd, timerfd, fcntl ---- */

#define AF_UNIX 1
#define SOCK_STREAM 1
#define SOCK_NONBLOCK O_NONBLOCK
#define SOCK_CLOEXEC O_CLOEXEC
#define SOCK_NAME_MAX 32
#define SOMAXCONN 8
#define SHUT_RD 0
#define SHUT_WR 1
#define SHUT_RDWR 2
#define SOL_SOCKET 1
#define SCM_RIGHTS 1
#define SCM_MAX_FD 16

struct sockaddr_un {
    uint16_t sun_family;
    char sun_path[SOCK_NAME_MAX];   /* abstract name, NUL terminated */
};

struct iovec {
    void *iov_base;
    size_t iov_len;
};

struct msghdr {
    void *msg_name;                 /* unused */
    uint32_t msg_namelen;
    struct iovec *msg_iov;
    size_t msg_iovlen;
    void *msg_control;              /* cmsghdr records */
    size_t msg_controllen;
    int msg_flags;
};

struct cmsghdr {
    size_t cmsg_len;                /* header plus data */
    int cmsg_level;
    int cmsg_type;
};

#define CMSG_ALIGN(n) (((n) + 7) & ~(size_t)7)
#define CMSG_SPACE(n) (CMSG_ALIGN(sizeof(struct cmsghdr)) + CMSG_ALIGN(n))
#define CMSG_LEN(n) (CMSG_ALIGN(sizeof(struct cmsghdr)) + (n))
#define CMSG_DATA(c) ((unsigned char *)(c) + CMSG_ALIGN(sizeof(struct cmsghdr)))
#define CMSG_FIRSTHDR(m) ((m)->msg_controllen >= sizeof(struct cmsghdr) ? (struct cmsghdr *)(m)->msg_control : NULL)
#define CMSG_NXTHDR(m, c) \
    ((unsigned char *)(c) + CMSG_ALIGN((c)->cmsg_len) + sizeof(struct cmsghdr) <= \
             (unsigned char *)(m)->msg_control + (m)->msg_controllen \
         ? (struct cmsghdr *)((unsigned char *)(c) + CMSG_ALIGN((c)->cmsg_len)) : NULL)
#define MSG_CTRUNC 8

#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define F_DUPFD_CLOEXEC 1030
#define FD_CLOEXEC 1

#define MFD_CLOEXEC 1
#define EFD_NONBLOCK O_NONBLOCK
#define EFD_CLOEXEC O_CLOEXEC
#define TFD_NONBLOCK O_NONBLOCK
#define TFD_CLOEXEC O_CLOEXEC

/* Timer descriptors count in milliseconds. */
struct timerfd_spec {
    uint64_t initial_ms;            /* 0 disarms */
    uint64_t interval_ms;           /* 0 for a single expiration */
};
