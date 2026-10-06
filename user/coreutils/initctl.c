/* initctl: ask init about its entries and control them.
 *
 *     initctl [list]
 *     initctl status|start|stop|restart NAME
 *     initctl reload [FILE]
 *     initctl poweroff|reboot|halt
 *
 * The request is one line on the abstract socket "init" (init_request of
 * minios/init.h); the reply starts with "ok" or "error: message" followed
 * by the output. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <minios/init.h>

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
    static char reply[4096];
    long n = init_request(request, reply, sizeof reply);
    if (n < 0) {
        fprintf(stderr, "initctl: cannot reach init: %s\n", strerror(errno));
        return 1;
    }
    size_t got = (size_t)n;
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
