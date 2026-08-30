/* M13 shutdown test, started as init: write a file on the disk root,
 * close it and shut the system down. The host checks the image. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/reboot.h>
#include <sys/stat.h>

int main(void)
{
    int fd = open("/persist.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        printf("shutdowntest: open: %s\n", strerror(errno));
        return 1;
    }
    const char msg[] = "written before shutdown\n";
    if (write(fd, msg, sizeof msg - 1) != (ssize_t)(sizeof msg - 1)) {
        printf("shutdowntest: write failed\n");
        return 1;
    }
    close(fd);
    if (mkdir("/persist.d", 0755) < 0) {
        printf("shutdowntest: mkdir: %s\n", strerror(errno));
        return 1;
    }
    printf("shutdowntest: shutting down\n");
    fflush(stdout);
    reboot(RB_POWER_OFF);
    printf("shutdowntest: reboot returned: %s\n", strerror(errno));
    return 1;
}
