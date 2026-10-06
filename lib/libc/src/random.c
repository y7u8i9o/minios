/* Random bytes of the kernel generator (sys/random.h, getentropy in
 * unistd.h). */
#include <sys/random.h>
#include <errno.h>
#include <unistd.h>
#include <minios/syscall.h>

ssize_t getrandom(void *buf, size_t len, unsigned flags)
{
    return (ssize_t)syscall3(SYS_getrandom, buf, len, flags);
}

int getentropy(void *buf, size_t len)
{
    if (len > 256) {
        errno = EIO;
        return -1;
    }
    unsigned char *p = buf;
    while (len) {
        ssize_t n = getrandom(p, len, 0);
        if (n < 0)
            return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}
