/* startgui [program]: start the desktop session (X12, the panel, the
 * desktop), run the program (the terminal by default) and stop the
 * session on logout. The audio server is a service of init and lives
 * across sessions. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include <minios/conf.h>

/* export_locale sets LANG from the lang setting of the desktop
 * configuration, and LC_NUMERIC, LC_TIME and LC_MONETARY from its formats
 * setting, for every program of the session (docs/design/desktop.md). */
static void export_locale(void)
{
    char path[256], line[256], lang[64] = "", formats[64] = "";
    FILE *f = fopen(conf_read_path(path, sizeof path), "r");
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = '\0';
        if (strncmp(line, "lang=", 5) == 0)
            strlcpy(lang, line + 5, sizeof lang);
        else if (strncmp(line, "formats=", 8) == 0)
            strlcpy(formats, line + 8, sizeof formats);
    }
    fclose(f);
    if (lang[0])
        setenv("LANG", lang, 1);
    if (formats[0]) {
        setenv("LC_NUMERIC", formats, 1);
        setenv("LC_TIME", formats, 1);
        setenv("LC_MONETARY", formats, 1);
    }
}

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
    export_locale();
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
