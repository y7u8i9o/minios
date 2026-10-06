/* notify-send: post a desktop notification (docs/design/notifications.md).
 *
 *   notify-send [-a app] [-i icon] [-u low|normal|critical] [-t ms]
 *               [-r number] [-A key=label]... [-p] [-w] summary [body]
 *
 * -p prints the number of the notification. -w waits until the
 * notification closes. While it waits, it prints "action KEY" for each
 * action that the user invokes. When the notification closes, it prints
 * "closed REASON", where REASON is expired, dismissed, closed or removed. */
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <gui/notify.h>

static int finished;

static void usage(void)
{
    fprintf(stderr, "usage: notify-send [-a app] [-i icon] [-u low|normal|critical] [-t ms] [-r number]\n"
                    "                   [-A key=label]... [-p] [-w] summary [body]\n");
    exit(2);
}

static void on_event(void *arg, uint32_t number, const char *key, enum notify_closed reason)
{
    static const char *const reasons[] = { "unknown", "expired", "dismissed", "closed", "removed" };
    if (key) {
        printf("action %s\n", key);
    } else {
        printf("closed %s\n", reason <= NOTIFY_REMOVED ? reasons[reason] : reasons[0]);
        finished = 1;
    }
    fflush(stdout);
}

int main(int argc, char **argv)
{
    struct notify_spec s;
    notify_spec_init(&s, "notify-send", NULL, NULL);
    struct notify_action actions[3];
    char *action_text[3];
    int nactions = 0, print = 0, wait = 0, opt;
    while ((opt = getopt(argc, argv, "a:i:u:t:r:A:pw")) != -1) {
        switch (opt) {
        case 'a':
            s.app_name = optarg;
            break;
        case 'i':
            s.icon = optarg;
            break;
        case 'u':
            if (strcmp(optarg, "low") == 0)
                s.urgency = NOTIFY_LOW;
            else if (strcmp(optarg, "normal") == 0)
                s.urgency = NOTIFY_NORMAL;
            else if (strcmp(optarg, "critical") == 0)
                s.urgency = NOTIFY_CRITICAL;
            else
                usage();
            break;
        case 't':
            s.timeout_ms = atoi(optarg);
            break;
        case 'r':
            s.replaces = (uint32_t)strtoul(optarg, NULL, 10);
            break;
        case 'A': {
            char *eq = strchr(optarg, '=');
            if (nactions == 3 || !eq || eq == optarg)
                usage();
            action_text[nactions] = strdup(optarg);
            if (!action_text[nactions])
                return 1;
            action_text[nactions][eq - optarg] = '\0';
            actions[nactions].key = action_text[nactions];
            actions[nactions].label = action_text[nactions] + (eq - optarg) + 1;
            nactions++;
            break;
        }
        case 'p':
            print = 1;
            break;
        case 'w':
            wait = 1;
            break;
        default:
            usage();
        }
    }
    if (optind >= argc || argc - optind > 2)
        usage();
    s.summary = argv[optind];
    s.body = optind + 1 < argc ? argv[optind + 1] : NULL;
    s.actions = actions;
    s.nactions = nactions;

    struct notify_client *c = notify_client_connect();
    if (!c) {
        fprintf(stderr, "notify-send: notifyd is not running\n");
        return 1;
    }
    long number = notify_client_post(c, &s, wait ? on_event : NULL, NULL);
    if (number < 0) {
        fprintf(stderr, "notify-send: %s\n", strerror((int)-number));
        notify_client_disconnect(c);
        return 1;
    }
    if (print) {
        printf("%ld\n", number);
        fflush(stdout);
    }
    while (wait && !finished) {
        struct pollfd pf = { notify_client_fd(c), POLLIN, 0 };
        if (poll(&pf, 1, -1) < 0)
            continue;
        if (notify_client_dispatch(c) < 0) {
            fprintf(stderr, "notify-send: notifyd stopped\n");
            notify_client_disconnect(c);
            return 1;
        }
    }
    notify_client_disconnect(c);
    return 0;
}
