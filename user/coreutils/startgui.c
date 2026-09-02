/* startgui [program]: start X12 and the panel, run the
 * program (the terminal by default) and stop both when it exits. */
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>

static pid_t spawn(const char *path)
{
    pid_t pid = fork();
    if (pid == 0) {
        char *const args[] = { (char *)path, NULL };
        execvp(path, args);
        perror(path);
        _exit(127);
    }
    return pid;
}

int main(int argc, char **argv)
{
    pid_t server = spawn("x12");
    sleep_ms(400);
    pid_t panel = spawn("panel");
    sleep_ms(200);
    pid_t desktop = spawn("desktop");
    sleep_ms(200);
    pid_t client = spawn(argc > 1 ? argv[1] : "term");
    /* The session lasts while the compositor and the panel run; the
     * program may exit (other programs are started from the panel).
     * "Log out" in the panel's menu ends the panel and so the session;
     * a crashed X12 server ends it too. */
    int status = 0, restarts = 0;
    for (;;) {
        pid_t done = waitpid(-1, &status, 0);
        if (done < 0)
            break;
        if (done == client) {
            client = -1;
            continue;
        }
        if (done == server)
            break;
        if (done == desktop) {
            if (++restarts > 3)
                break;
            fprintf(stderr, "startgui: desktop ended with status 0x%x, restarting\n", status);
            sleep_ms(200);
            desktop = spawn("desktop");
            continue;
        }
        if (done == panel) {
            /* A normal exit is "Log out". A crash or a protocol error
             * (abnormal status) gets the panel restarted, so the
             * session survives a panel defect. */
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
                break;
            if (++restarts > 3)
                break;
            fprintf(stderr, "startgui: panel ended with status 0x%x, restarting\n", status);
            sleep_ms(200);
            panel = spawn("panel");
        }
    }
    if (client > 0) {
        kill(client, SIGTERM);
        waitpid(client, NULL, 0);
    }
    kill(panel, SIGTERM);
    waitpid(panel, NULL, 0);
    kill(desktop, SIGTERM);
    waitpid(desktop, NULL, 0);
    kill(server, SIGTERM);
    waitpid(server, NULL, 0);
    return 0;
}
