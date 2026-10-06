#pragma once
/* Binary interface shared between the kernel and libc: structures crossing
 * the system call boundary. Only fixed width types are used here. */
#include <stddef.h>
#include <stdint.h>

/* File type bits in st_mode. */
#define S_IFMT   0170000
#define S_IFDIR  0040000
#define S_IFREG  0100000
#define S_IFCHR  0020000
#define S_IFBLK  0060000
#define S_IFIFO  0010000
#define S_IFLNK  0120000
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISCHR(m) (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m) (((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)
/* The set user id, set group id and sticky bits. */
#define S_ISUID  04000
#define S_ISGID  02000
#define S_ISVTX  01000

/* Supplementary groups a process may carry (setgroups, getgroups). */
#define NGROUPS_MAX 16

struct timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

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
    struct timespec st_mtim;        /* modification time; the filesystems retain seconds */
};
#define st_mtime st_mtim.tv_sec
/* minios retains no access or change time. Both names report the
 * modification time, which leaves the structure unchanged for programs
 * built before U5. */
#define st_atim st_mtim
#define st_ctim st_mtim
#define st_atime st_mtim.tv_sec
#define st_ctime st_mtim.tv_sec

/* The *at system calls: the directory descriptor meaning the working
 * directory, the flag that makes fstatat and utimensat act on a symbolic
 * link itself, and the two special tv_nsec values of a utimensat
 * timespec. */
#define AT_FDCWD   (-100)
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_EACCESS 0x200                /* faccessat: effective ids */
#define AT_REMOVEDIR        0x200
#define UTIME_NOW  ((1l << 30) - 1l)
#define UTIME_OMIT ((1l << 30) - 2l)

/* Directory entry types. */
#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8
#define DT_LNK     10

#define NAME_MAX 255
/* A lookup follows at most this many symbolic links before it fails with
 * ELOOP. */
#define SYMLOOP_MAX 40

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
#define O_NOFOLLOW  0x20000         /* fail with ELOOP when the last component is a symbolic link */
#define O_NONBLOCK  0x800
#define O_CLOEXEC   0x80000
#define O_NOCTTY    0x100           /* accepted, minios has no controlling terminal to acquire */
#define O_DSYNC     0x1000          /* accepted, writes reach the block cache at once */
#define O_SYNC      0x101000

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
#define MAP_HUGETLB   0x40000
#define MAP_FAILED ((void *)-1)
/* madvise advice (M38) */
#define MADV_NORMAL     0
#define MADV_RANDOM     1
#define MADV_SEQUENTIAL 2
#define MADV_WILLNEED   3
#define MADV_DONTNEED   4
#define MADV_FREE       8
#define MADV_DONTFORK   10
#define MADV_DOFORK     11
#define MADV_HUGEPAGE   14
#define MADV_NOHUGEPAGE 15
/* msync flags */
#define MS_ASYNC      1
#define MS_INVALIDATE 2
#define MS_SYNC       4

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
#define SIGCONT  18
#define SIGSTOP  19
#define SIGTSTP  20
#define SIGTTIN  21
#define SIGTTOU  22
#define SIGXCPU  24     /* CPU time limit exceeded (M40) */
#define SIGXFSZ  25     /* file size limit exceeded (M40) */
#define SIGWINCH 28
#define NSIG     32

#define SIG_DFL ((void (*)(int))0)
#define SIG_IGN ((void (*)(int))1)
#define SIG_ERR ((void (*)(int))-1)

#define SA_NOCLDSTOP 0x00000001
#define SA_SIGINFO  0x00000004      /* the handler is sa_sigaction (U5) */
#define SA_ONSTACK  0x08000000      /* accepted, minios has no alternate stack */
#define SA_RESTART  0x10000000      /* accepted, interrupted calls still fail with EINTR */
#define SA_NODEFER  0x40000000
#define SA_RESETHAND 0x80000000
#define SA_RESTORER 0x04000000

/* The values of si_code, which tell a signal from kill or the kernel and
 * give the reasons of SIGCHLD, SIGFPE and SIGSEGV. */
#define SI_USER     0
#define SI_KERNEL   0x80
#define CLD_EXITED  1
#define CLD_KILLED  2
#define CLD_DUMPED  3
#define CLD_TRAPPED 4
#define CLD_STOPPED 5
#define CLD_CONTINUED 6
/* The si_code values of SIGFPE and SIGSEGV. minios posts these signals
 * with SI_KERNEL. */
#define FPE_INTDIV  1
#define FPE_INTOVF  2
#define FPE_FLTDIV  3
#define FPE_FLTOVF  4
#define FPE_FLTUND  5
#define FPE_FLTRES  6
#define FPE_FLTINV  7
#define FPE_FLTSUB  8
#define SEGV_MAPERR 1
#define SEGV_ACCERR 2

/* The information a handler with SA_SIGINFO receives (U5). si_pid and
 * si_uid name the sending process for SI_USER. */
typedef struct {
    int32_t si_signo;
    int32_t si_errno;
    int32_t si_code;
    int32_t si_pid;
    uint32_t si_uid;
    int32_t si_status;
    uint64_t si_addr;
    uint64_t si_value;
} siginfo_t;

#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

typedef uint64_t sigset_t;

struct sigaction {
    union {
        void (*sa_handler)(int);
        void (*sa_sigaction)(int, siginfo_t *, void *);
    };
    sigset_t sa_mask;
    int sa_flags;
    void (*sa_restorer)(void);
};

/* reboot */
#define RB_POWER_OFF 0
#define RB_AUTOBOOT  1
#define RB_HALT      2

/* Terminal control. The line discipline interprets ISIG, ICANON and ECHO
 * of c_lflag. The other flags, the speeds and the control characters are
 * stored and reported as set, for programs that save and restore them
 * (U5 of docs/plan/multiuser.md). The values are those of Linux. */
#define ISIG    0x0001  /* control C sends SIGINT */
#define ICANON  0x0002  /* line editing, deliver whole lines */
#define ECHO    0x0008  /* echo typed characters */
#define ECHOE   0x0010
#define ECHOK   0x0020
#define ECHONL  0x0040
#define NOFLSH  0x0080
#define TOSTOP  0x0100
#define IEXTEN  0x8000

#define IGNBRK  0x0001
#define BRKINT  0x0002
#define IGNPAR  0x0004
#define PARMRK  0x0008
#define INPCK   0x0010
#define ISTRIP  0x0020
#define INLCR   0x0040
#define IGNCR   0x0080
#define ICRNL   0x0100
#define IXON    0x0400
#define IXANY   0x0800
#define IXOFF   0x1000
#define IMAXBEL 0x2000

#define OPOST   0x0001
#define ONLCR   0x0004
#define OCRNL   0x0008
#define ONOCR   0x0010
#define ONLRET  0x0020

#define CSIZE   0x0030
#define CS5     0x0000
#define CS6     0x0010
#define CS7     0x0020
#define CS8     0x0030
#define CSTOPB  0x0040
#define CREAD   0x0080
#define PARENB  0x0100
#define PARODD  0x0200
#define HUPCL   0x0400
#define CLOCAL  0x0800

#define B0      0
#define B50     1
#define B75     2
#define B110    3
#define B134    4
#define B150    5
#define B200    6
#define B300    7
#define B600    8
#define B1200   9
#define B1800   10
#define B2400   11
#define B4800   12
#define B9600   13
#define B19200  14
#define B38400  15

#define VINTR    0
#define VQUIT    1
#define VERASE   2
#define VKILL    3
#define VEOF     4
#define VTIME    5
#define VMIN     6
#define VSTART   8
#define VSTOP    9
#define VSUSP    10
#define VEOL     11
#define VREPRINT 12
#define VDISCARD 13
#define VWERASE  14
#define VLNEXT   15
#define VEOL2    16
#define NCCS     20
#define _POSIX_VDISABLE 0

struct termios {
    uint32_t c_lflag;           /* first, as in the subset before U5 */
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint8_t c_cc[NCCS];
    uint32_t c_ispeed;
    uint32_t c_ospeed;
};

#define TCGETS   0x5401
#define TCSETS   0x5402
#define TCFLSH   0x540B         /* TCIFLUSH, TCOFLUSH or TCIOFLUSH */
#define TCIFLUSH  0
#define TCOFLUSH  1
#define TCIOFLUSH 2
#define TIOCGWINSZ 0x5413
/* Read the partition table of a disk again (the number of Linux), on its
 * device file. It fails with EBUSY while the root or swap lies on the
 * disk (docs/design/block.md). */
#define BLKRRPART 0x125f

struct winsize {
    uint16_t ws_row;
    uint16_t ws_col;
};

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
    uint8_t scale;          /* integer UI scale (video=WxH@N or FBIO_SET_MODE), 1 by default */
    uint8_t pad;
    uint32_t caps;          /* FB_CAP_* */
    uint32_t size;          /* bytes mmap may map; the whole GPU buffer with FB_CAP_SET_MODE */
};
/* With FB_CAP_FLUSH the mapping is ordinary cacheable RAM, which the device
 * reads only during a flush. Without it the mapping is video memory, mapped
 * write combining, which the display scans continuously. */
#define FB_CAP_FLUSH    1   /* changes reach the display with FBIO_FLUSH (virtio-gpu) */
#define FB_CAP_SET_MODE 2   /* FBIO_SET_MODE changes the resolution at run time */
#define FB_CAP_FLUSH_RECTS 4 /* FBIO_FLUSH_RECTS flushes several rectangles in one call */
#define FB_CAP_CURSOR   8   /* FBIO_CURSOR_SET and FBIO_CURSOR_MOVE show a cursor above the framebuffer */

struct fb_rect {
    int32_t x, y, w, h;
};
/* FBIO_FLUSH_RECTS: count rectangles, at most FB_FLUSH_MAX. flags is 0. */
#define FB_FLUSH_MAX 32
struct fb_flush_rects {
    uint32_t count;
    uint32_t flags;
    struct fb_rect rects[FB_FLUSH_MAX];
};
struct fb_mode {
    uint32_t width, height; /* pixels; width * height * 4 must fit in fb_info.size */
    uint32_t scale;         /* 1..4, reported back in fb_info.scale */
};
#define FBIOGET_INFO 0x4600
#define FBIO_ACQUIRE 0x4601  /* stop the kernel text console from drawing */
#define FBIO_RELEASE 0x4602  /* restore the text console */
#define FBIO_FLUSH   0x4603  /* struct fb_rect: push a rectangle to the display, no-op without FB_CAP_FLUSH */
#define FBIO_SET_MODE 0x4604 /* struct fb_mode: display owner only; the mapping remains valid, geometry changes */
#define FBIOGET_DISPLAY 0x4605 /* struct fb_display: the last size request of the host */
/* struct fb_flush_rects: push several rectangles to the display. While a
 * file owns the display, only that file may call it (EPERM otherwise). A
 * count above FB_FLUSH_MAX or nonzero flags give EINVAL. No-op without
 * FB_CAP_FLUSH. */
#define FBIO_FLUSH_RECTS 0x4606

/* The cursor of a device with FB_CAP_CURSOR (G9 of
 * docs/plan/compositor-performance.md). The device draws the image above
 * the framebuffer. The image does not change the framebuffer and needs no
 * flush. Pixels are 0xAARRGGBB with straight alpha, in rows of
 * FB_CURSOR_MAX pixels. The hotspot (hot_x, hot_y) is a pixel of the
 * image. The position (x, y) is the screen pixel under the hotspot. All
 * values are device pixels. Only the display owner may call the two
 * requests (EPERM otherwise). The cursor disappears when the owner
 * releases the display. */
#define FB_CURSOR_MAX 64
struct fb_cursor {
    uint32_t width, height;     /* 1..FB_CURSOR_MAX, or width 0 to hide the cursor */
    uint32_t hot_x, hot_y;      /* below width and height */
    int32_t x, y;
    uint32_t pixels[FB_CURSOR_MAX * FB_CURSOR_MAX];
};
struct fb_cursor_pos {
    int32_t x, y;
};
#define FBIO_CURSOR_SET 0x4607  /* struct fb_cursor: set the image and the position, or hide */
#define FBIO_CURSOR_MOVE 0x4608 /* struct fb_cursor_pos: move a shown cursor */

/* A size request of the host display (V3 of docs/plan/release-0.6.0.md).
 * The host sends one when the window of the virtual display changes its
 * size. /dev/fb0 reports POLLIN to the display owner while a request is
 * unread. FBIOGET_DISPLAY marks the request read for the owner. */
struct fb_display {
    uint32_t width, height; /* pixels, 0 before the first request */
    uint32_t serial;        /* counts the requests since boot */
};

/* Raw PCM audio devices.  Clients normally use audiod rather than opening
 * /dev/pcmN directly.  Structures have fixed-width fields so the ABI can be
 * extended without depending on compiler enum sizes. */
#define AUDIO_ABI_VERSION 2
#define AUDIO_CAP_PLAYBACK (1u << 0)
#define AUDIO_CAP_CAPTURE  (1u << 1)    /* the read side: AUDIO_*CAPTURE* requests */
#define AUDIO_FORMAT_S16_LE (1u << 0)
#define AUDIO_RATE_48000    (1u << 0)

#define AUDIO_STATE_CLOSED   0
#define AUDIO_STATE_OPEN     1
#define AUDIO_STATE_PREPARED 2
#define AUDIO_STATE_RUNNING  3
#define AUDIO_STATE_ERROR    4

struct audio_info {
    uint32_t abi_version;
    uint32_t capabilities;
    uint32_t formats;
    uint32_t rates;
    uint32_t channels_min;
    uint32_t channels_max;
    uint32_t period_frames_min;
    uint32_t period_frames_max;
    uint32_t periods_min;
    uint32_t periods_max;
};

struct audio_params {
    uint32_t format;
    uint32_t rate;
    uint32_t channels;
    uint32_t period_frames;
    uint32_t periods;
};

/* Playback: queued_frames are submitted and not yet played, xruns count
 * empty rings while running.  Capture: queued_frames are captured and
 * not yet read, played_frames counts captured frames, xruns count
 * periods the device could not fill because the reader was late. */
struct audio_status {
    uint32_t state;
    uint32_t queued_frames;
    uint64_t played_frames;
    uint32_t xruns;
    int32_t last_error;
};

#define AUDIO_GET_INFO    0x4100
#define AUDIO_SET_PARAMS  0x4101
#define AUDIO_PREPARE     0x4102
#define AUDIO_START       0x4103
#define AUDIO_DROP        0x4104
#define AUDIO_DRAIN       0x4105
#define AUDIO_GET_STATUS  0x4106
/* The capture stream of a device with AUDIO_CAP_CAPTURE.  Reads are
 * exactly one configured period; poll(POLLIN) reports a full period. */
#define AUDIO_SET_CAPTURE_PARAMS 0x4107
#define AUDIO_CAPTURE_PREPARE    0x4108
#define AUDIO_CAPTURE_START      0x4109
#define AUDIO_CAPTURE_DROP       0x410a
#define AUDIO_GET_CAPTURE_STATUS 0x410b

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
#define TIOCGPTN   0x5430
/* /dev/net (N10): interface configuration and the ICMP echo interface.
 * Addresses are host order. address 0 detaches the address, the mask and
 * the gateway of the interface but retains it as the broadcast interface. */
#define NETIOC_CONFIGURE 0x4e01  /* struct net_config * */
#define NETIOC_PING      0x4e02  /* struct net_ping *, returns 0 or -errno */
/* NETIOC_ARP_PROBE takes a struct net_arp_probe and implements RFC 5227
 * address conflict detection (N16). It sends one ARP probe with sender
 * address 0, or with announce set one announcement whose sender and target
 * addresses are equal, for address on the named Ethernet interface, and
 * then waits up to wait_ms (at most 10000) for another host to claim the
 * address. It returns 0, or -EADDRINUSE with that host's hardware address
 * in mac. */
#define NETIOC_ARP_PROBE 0x4e03
struct net_config {
    char name[16];
    uint32_t address, mask, gateway;
};
/* The address is in host order, and the kernel writes mac. */
struct net_arp_probe {
    char name[16];
    uint32_t address;
    uint32_t wait_ms;
    uint32_t announce;
    uint8_t mac[6];
};
struct net_ping {
    uint32_t address;
    uint32_t timeout_ms;   /* at most 60000 */
    uint16_t sequence;
    uint16_t size;         /* payload bytes, at most 1400 */
    uint32_t rtt_ms;       /* out */
    uint8_t ttl;           /* out */
};   /* pseudo terminal master: index of the slave */

/* ---- M23: sockets, descriptor passing, memfd, eventfd, timerfd, fcntl ---- */

#define AF_UNSPEC 0
#define AF_UNIX 1
#define PF_UNSPEC AF_UNSPEC
#define PF_UNIX AF_UNIX
#define AF_LOCAL AF_UNIX
#define PF_LOCAL AF_UNIX
#define AF_INET 2
#define PF_INET AF_INET
#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define SOCK_RAW 3
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

typedef uint32_t socklen_t;

struct sockaddr_un {
    uint16_t sun_family;
    char sun_path[SOCK_NAME_MAX];   /* abstract name, NUL terminated */
};

/* ---- N01: Internet addresses, message flags and socket options ---- */

#define IPPROTO_IP   0
#define IPPROTO_ICMP 1
#define IPPROTO_TCP  6
#define IPPROTO_UDP  17

/* Both fields are in network byte order. */
struct in_addr {
    uint32_t s_addr;
};

struct sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;
    struct in_addr sin_addr;
    uint8_t sin_zero[8];
};

#define INADDR_ANY       0x00000000u
#define INADDR_LOOPBACK  0x7f000001u
#define INADDR_BROADCAST 0xffffffffu
#define INADDR_NONE      0xffffffffu

/* Large enough for every address family, aligned for any of them. */
struct sockaddr_storage {
    uint16_t ss_family;
    uint8_t ss_pad1[6];
    uint64_t ss_align;
    uint8_t ss_pad2[112];
};

/* Message flags of sendmsg and recvmsg. A flag a backend does not
 * implement is rejected with EOPNOTSUPP, an unknown one with EINVAL. */
#define MSG_OOB      0x1
#define MSG_PEEK     0x2
#define MSG_TRUNC    0x20
#define MSG_DONTWAIT 0x40
#define MSG_EOR      0x80
#define MSG_WAITALL  0x100
#define MSG_NOSIGNAL 0x4000

/* Options at SOL_SOCKET level; the values are int unless noted. */
#define SO_DEBUG     1
#define SO_REUSEADDR 2
#define SO_TYPE      3
#define SO_ERROR     4
#define SO_BROADCAST 6
#define SO_SNDBUF    7
#define SO_RCVBUF    8
#define SO_KEEPALIVE 9
#define SO_PEERCRED  17                 /* struct ucred, AF_UNIX, read only (U4) */
#define SO_PROTOCOL  38
#define SO_DOMAIN    39

/* The process at the other end of an AF_UNIX connection, recorded when
 * the connection was made: the connecting process, or the process that
 * called listen. */
struct ucred {
    int32_t pid;
    uint32_t uid;
    uint32_t gid;
};

struct iovec {
    void *iov_base;
    size_t iov_len;
};

struct msghdr {
    void *msg_name;                 /* address: sent to, or received from (N01) */
    socklen_t msg_namelen;
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
/* Record locks (U5) are accepted and not enforced. minios retains none, and
 * F_GETLK always reports the region unlocked. */
#define F_GETLK  5
#define F_SETLK  6
#define F_SETLKW 7
#define F_RDLCK  0
#define F_WRLCK  1
#define F_UNLCK  2

struct flock {
    int16_t l_type;
    int16_t l_whence;
    int64_t l_start;
    int64_t l_len;
    int32_t l_pid;
};
#define FD_CLOEXEC 1

#define MFD_CLOEXEC 1
#define EFD_NONBLOCK O_NONBLOCK
#define EFD_CLOEXEC O_CLOEXEC
#define TFD_NONBLOCK O_NONBLOCK
#define TFD_CLOEXEC O_CLOEXEC

/* ---- M35: threads and time ---- */

/* futex(addr, op, value, timeout_ms): FUTEX_WAIT sleeps while *addr ==
 * value (0 woken, -ETIMEDOUT, -EAGAIN when the value differs, -EINTR);
 * FUTEX_WAKE wakes up to value waiters and returns how many. Private to
 * the calling process. timeout_ms 0 waits without limit. */
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1

#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

/* Timer descriptors count in milliseconds. */
struct timerfd_spec {
    uint64_t initial_ms;            /* 0 disarms */
    uint64_t interval_ms;           /* 0 for a single expiration */
};

/* ---- M40: resource limits and usage ---- */

#define RLIMIT_CPU     0    /* seconds of CPU time */
#define RLIMIT_FSIZE   1    /* bytes a file may grow to */
#define RLIMIT_DATA    2    /* bytes of heap */
#define RLIMIT_STACK   3    /* bytes of main stack, applied by exec */
#define RLIMIT_CORE    4    /* stored only */
#define RLIMIT_RSS     5    /* stored only */
#define RLIMIT_NPROC   6    /* user processes */
#define RLIMIT_NOFILE  7    /* descriptor slots, at most OPEN_MAX */
#define RLIMIT_MEMLOCK 8    /* stored only */
#define RLIMIT_AS      9    /* bytes of address space (sum of the regions) */
#define RLIMIT_NLIMITS 10
#define RLIM_INFINITY  (~0UL)

struct rlimit {
    uint64_t rlim_cur;      /* soft limit, the one enforced */
    uint64_t rlim_max;      /* hard limit, ceiling for rlim_cur */
};

struct abi_timeval {
    int64_t tv_sec;
    int64_t tv_usec;
};

#define RUSAGE_SELF      0
#define RUSAGE_CHILDREN (-1)
#define RUSAGE_THREAD    1

struct rusage {
    struct abi_timeval ru_utime;    /* user CPU time */
    struct abi_timeval ru_stime;    /* system CPU time */
    int64_t ru_maxrss;              /* resident pages in KiB, counted when asked */
    int64_t ru_ixrss;
    int64_t ru_idrss;
    int64_t ru_isrss;
    int64_t ru_minflt;              /* faults served from memory */
    int64_t ru_majflt;              /* faults that read swap or a file */
    int64_t ru_nswap;
    int64_t ru_inblock;
    int64_t ru_oublock;
    int64_t ru_msgsnd;
    int64_t ru_msgrcv;
    int64_t ru_nsignals;
    int64_t ru_nvcsw;               /* voluntary context switches */
    int64_t ru_nivcsw;              /* involuntary context switches */
};

/* ---- M48: full system profiler, /dev/profile ---- */

/* The device delivers a stream of variable length events, one record per
 * observation, ordered by time across the per CPU rings that produce them.
 * Every record starts with the same header, so a reader that does not know
 * a type can still skip it with size. */

#define PROF_MAX_FRAMES 32      /* frames one chain can contain */

/* Event types. A type is also a bit position in the class mask. */
#define PROF_EV_SAMPLE 0        /* timer sample of the running thread */
#define PROF_EV_BLOCK  1        /* thread left a CPU; chain is where it stopped */
#define PROF_EV_RUN    2        /* thread returned to a CPU */
#define PROF_EV_ALLOC  3        /* kernel heap allocation */
#define PROF_EV_FREE   4        /* kernel heap release */
#define PROF_EV_IO     5        /* completed block or file transfer */
#define PROF_EV_TYPES  6
#define PROF_EV_PAD    255      /* fills a ring to its wrap point, never delivered */

#define PROF_MASK(type) (1u << (type))
#define PROF_MASK_CPU   PROF_MASK(PROF_EV_SAMPLE)
#define PROF_MASK_SCHED (PROF_MASK(PROF_EV_BLOCK) | PROF_MASK(PROF_EV_RUN))
#define PROF_MASK_HEAP  (PROF_MASK(PROF_EV_ALLOC) | PROF_MASK(PROF_EV_FREE))
#define PROF_MASK_IO    PROF_MASK(PROF_EV_IO)
#define PROF_MASK_ALL   (PROF_MASK_CPU | PROF_MASK_SCHED | PROF_MASK_HEAP | PROF_MASK_IO)

#define PROF_FLAG_USER     0x01 /* the event was raised in user mode */
#define PROF_FLAG_KUSER    0x02 /* the chain crosses into user frames */
#define PROF_FLAG_WRITE    0x04 /* IO: a write */
#define PROF_FLAG_BLOCKDEV 0x08 /* IO: a block device, otherwise a file */
#define PROF_FLAG_PREEMPT  0x10 /* BLOCK: preempted while runnable */
#define PROF_FLAG_EXIT     0x20 /* BLOCK: the thread will not run again */
#define PROF_FLAG_TRUNC    0x40 /* the chain reached PROF_MAX_FRAMES */

/* Separates the kernel frames of a chain from the user frames of the entry
 * that led into the kernel. It is never a valid address. */
#define PROF_FRAME_BOUNDARY 0xffffffffffffffffULL

/* One observation. chain contains depth addresses, innermost first.
 * a and b depend on the type:
 *   SAMPLE  a: 0                     b: 0
 *   BLOCK   a: nanoseconds on CPU    b: thread state left behind
 *   RUN     a: nanoseconds off CPU   b: nanoseconds runnable before running
 *   ALLOC   a: address               b: bytes requested
 *   FREE    a: address               b: bytes, 0 when the size is unknown
 *   IO      a: bytes transferred     b: nanoseconds of latency */
struct prof_event {
    uint16_t size;              /* bytes of this record, a multiple of 8 */
    uint8_t type;
    uint8_t flags;
    uint16_t depth;
    uint16_t cpu;
    uint64_t time_ns;           /* monotonic, the same clock on every CPU */
    uint32_t pid, tid;
    uint64_t a, b;
    uint64_t chain[];
};

#define PROF_EVENT_HEADER ((unsigned)sizeof(struct prof_event))
#define PROF_EVENT_MAX (PROF_EVENT_HEADER + PROF_MAX_FRAMES * 8u)

/* What the engine records. A zero field retains the current setting. */
struct prof_config {
    uint32_t pid;               /* 0: every process */
    uint32_t divider;           /* timer ticks between samples, 1..1000 */
    uint32_t events;            /* PROF_MASK_* */
    uint32_t max_depth;         /* frames per chain, 1..PROF_MAX_FRAMES */
    uint32_t alloc_min;         /* smallest recorded allocation in bytes */
    uint32_t io_min_ns;         /* shortest recorded transfer */
};

struct prof_stats {
    uint64_t events;            /* recorded since PROF_START */
    uint64_t dropped;           /* lost because a ring was full */
    uint64_t pending;           /* bytes waiting to be read */
    uint64_t counts[PROF_EV_TYPES];
    uint32_t enabled;
    uint32_t pid;
    uint32_t divider;
    uint32_t event_mask;
    uint32_t max_depth;
    uint32_t ring;              /* bytes in one CPU ring */
    uint32_t cpus;
    uint32_t period_ns;         /* CPU time one sample stands for */
};

#define PROF_START       0x5000     /* arg: pid to profile, 0 for all */
#define PROF_STOP        0x5001
#define PROF_SET_DIVIDER 0x5002     /* arg: 1..1000 ticks */
#define PROF_GET_STATS   0x5003     /* arg: struct prof_stats * */
#define PROF_CONFIGURE   0x5004     /* arg: struct prof_config * */
