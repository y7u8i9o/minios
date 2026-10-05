/* Commands run by an application while its windows continue to handle
 * events, and commands run as root through sudo -A with askpass, the
 * authentication dialog of the desktop (docs/design/users.md). */
#include <gui/privilege.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#define MAX_ARGS 32

void askpass_state_path(char *buf, size_t size, uid_t uid, pid_t sudo_pid)
{
    snprintf(buf, size, "/tmp/.askpass-%d-%d", (int)uid, (int)sudo_pid);
}

/* The output of the command while it runs. */
struct run {
    int fd;
    char *out;
    size_t size, used;
    int done;
};

static void on_output(int fd, int revents, void *arg)
{
    struct run *r = arg;
    char discard[256];
    char *to = r->used + 1 < r->size ? r->out + r->used : discard;
    size_t room = r->used + 1 < r->size ? r->size - 1 - r->used : sizeof discard;
    ssize_t n = read(fd, to, room);
    if (n > 0 && to != discard)
        r->used += (size_t)n;
    if (n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN))
        r->done = 1;
}

/* A cancelled dialog leaves the word cancel in the state file of the run
 * of sudo. The file is removed. */
static int was_cancelled(pid_t sudo_pid)
{
    char path[64], word[8] = "";
    askpass_state_path(path, sizeof path, getuid(), sudo_pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    ssize_t n = read(fd, word, sizeof word - 1);
    close(fd);
    unlink(path);
    word[n > 0 ? n : 0] = '\0';
    return strncmp(word, "cancel", 6) == 0;
}

int app_run_command(struct app *a, char *const argv[], const char *input, char *out, size_t size, pid_t *child)
{
    /* The handler of an event that arrives while a command runs cannot
     * start a second one. */
    static int running;
    if (running)
        return -EBUSY;
    int in[2], res[2];
    if (pipe(in) < 0)
        return -errno;
    if (pipe(res) < 0) {
        int err = errno;
        close(in[0]);
        close(in[1]);
        return -err;
    }
    pid_t pid = fork();
    if (pid < 0) {
        int err = errno;
        close(in[0]);
        close(in[1]);
        close(res[0]);
        close(res[1]);
        return -err;
    }
    if (pid == 0) {
        dup2(in[0], 0);
        dup2(res[1], 1);
        dup2(res[1], 2);
        close(in[0]);
        close(in[1]);
        close(res[0]);
        close(res[1]);
        execv(argv[0], argv);
        _exit(127);
    }
    if (child)
        *child = pid;
    close(in[0]);
    close(res[1]);
    if (input && *input)
        write(in[1], input, strlen(input));
    close(in[1]);
    running = 1;
    struct run r = { res[0], out, size, 0, 0 };
    struct watch *w = app_watch_fd(a, res[0], POLLIN, on_output, &r);
    while (w && !r.done && app_step(a, -1))
        ;
    if (w)
        app_unwatch_fd(a, w);
    else
        while (!r.done)
            on_output(res[0], POLLIN, &r);
    close(res[0]);
    if (size)
        out[r.used < size ? r.used : size - 1] = '\0';
    int status, err = 0;
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR) {
            err = -ECHILD;
            break;
        }
    running = 0;
    if (err)
        return err;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -EINTR;
}

int app_run_privileged(struct app *a, const char *reason, char *const argv[], const char *input, char *out,
                       size_t size)
{
    if (geteuid() == 0)
        return app_run_command(a, argv, input, out, size, NULL);
    char *args[MAX_ARGS + 6];
    int n = 0;
    args[n++] = "/usr/bin/sudo";
    args[n++] = "-A";
    args[n++] = "-p";
    args[n++] = (char *)reason;
    args[n++] = "--";
    for (int i = 0; argv[i]; i++) {
        if (i == MAX_ARGS)
            return -E2BIG;
        args[n++] = argv[i];
    }
    args[n] = NULL;
    pid_t pid = 0;
    int r = app_run_command(a, args, input, out, size, &pid);
    if (pid > 0 && was_cancelled(pid))
        return -ECANCELED;
    return r;
}
