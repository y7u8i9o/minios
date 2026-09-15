/* initctl: ask init about its entries and control them.
 *
 *     initctl [list]
 *     initctl status|start|stop|restart NAME
 *     initctl reload [FILE]
 *     initctl poweroff|reboot|halt
 *
 * The request is one line on the abstract socket "init"; the reply
 * starts with "ok" or "error: message" followed by the output. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>

static void usage(void)
{
    fprintf(stderr, "usage: initctl [list | status NAME | start NAME | stop NAME | restart NAME |\n"
                    "                reload [FILE] | poweroff | reboot | halt]\n");
}

int main(int argc, char **argv)
{
    char request[256] = "";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage();
            return 0;
        }
        if (strlen(request) + strlen(argv[i]) + 2 >= sizeof request) {
            fprintf(stderr, "initctl: request too long\n");
            return 1;
        }
        if (i > 1)
            strcat(request, " ");
        strcat(request, argv[i]);
    }
    if (argc == 1)
        strcpy(request, "list");
    strcat(request, "\n");
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = { AF_UNIX, "init" };
    if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        fprintf(stderr, "initctl: cannot reach init: %s\n", strerror(errno));
        return 1;
    }
    size_t len = strlen(request);
    if (write(fd, request, len) != (ssize_t)len) {
        fprintf(stderr, "initctl: write: %s\n", strerror(errno));
        return 1;
    }
    static char reply[4096];
    size_t got = 0;
    for (;;) {
        ssize_t n = read(fd, reply + got, sizeof reply - 1 - got);
        if (n <= 0)
            break;
        got += (size_t)n;
        if (got == sizeof reply - 1)
            break;
    }
    close(fd);
    reply[got] = 0;
    if (got == 0) {
        fprintf(stderr, "initctl: no reply from init\n");
        return 1;
    }
    char *body = strchr(reply, '\n');
    if (body)
        body++;
    else
        body = reply + got;
    if (strncmp(reply, "ok", 2) == 0) {
        fputs(body, stdout);
        return 0;
    }
    /* The first line carries the diagnostic. */
    fprintf(stderr, "initctl: %.*s\n", (int)(body - reply - (body > reply ? 1 : 0)),
            strncmp(reply, "error: ", 7) == 0 ? reply + 7 : reply);
    fputs(body, stdout);
    return 1;
}
