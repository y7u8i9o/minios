/* init: process 1. Reads /etc/init.conf, runs the boot tasks in order,
 * supervises the services and the console session, answers initctl on
 * the abstract socket "init", reaps orphaned processes and performs the
 * orderly shutdown when asked with SIGUSR1 (power off), SIGUSR2 (reboot),
 * SIGHUP (halt) or an initctl request.
 *
 * The configuration has one entry per line (init.conf(5)):
 *
 *     env NAME=VALUE                   environment of every program
 *     task NAME [options] COMMAND...   run once, wait, then continue
 *     service NAME [options] COMMAND.. run in the background, restarted
 *     console NAME [options] COMMAND.. the session on the console, with
 *                                      its own process group and the
 *                                      terminal, restarted when it ends
 *
 * Options are if=PATH (start only when PATH exists), log=FILE (standard
 * output and error appended to FILE) and restart=always|never|failure.
 * A service that exits within QUICK_MS of its start QUICK_LIMIT times in
 * a row is left in the failed state until initctl starts it again.
 * Without a readable configuration the built-in table below applies, so
 * a damaged file still boots to a shell. */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/reboot.h>
#include <minios/abi.h>

#define CONFIG_PATH   "/etc/init.conf"
#define CONTROL_NAME  "init"
#define MAX_ENTRIES   32
#define MAX_ARGS      16
#define QUICK_MS      5000      /* an exit this soon after the start counts as quick */
#define QUICK_LIMIT   5         /* quick exits in a row before a service is given up */
#define RESTART_MS    1000      /* pause before restarting after a quick exit */
#define STOP_MS       3000      /* grace period between SIGTERM and SIGKILL */
#define REPLY_MAX     4096

enum kind { KIND_TASK, KIND_SERVICE, KIND_CONSOLE };
enum restart { RESTART_ALWAYS, RESTART_NEVER, RESTART_FAILURE };
enum state {
    STATE_WAITING,      /* not started yet, or due for a restart */
    STATE_RUNNING,
    STATE_STOPPING,     /* SIGTERM sent by request */
    STATE_STOPPED,      /* stopped by request, not restarted */
    STATE_DONE,         /* a task that exited with status 0 */
    STATE_FAILED,       /* a task that failed, or a service given up on */
    STATE_SKIPPED       /* the if= path does not exist */
};

struct entry {
    char name[24];
    enum kind kind;
    enum restart restart;
    char words[256];            /* the command words, NUL separated */
    int argc;
    char require[64];           /* if=PATH, empty when absent */
    char log[64];               /* log=FILE, empty when absent */
    enum state state;
    pid_t pid;                  /* 0 when no process runs */
    int starts;                 /* processes started so far */
    int quick;                  /* consecutive quick exits */
    int status;                 /* wait status of the last exit */
    uint64_t started_ms;
    uint64_t restart_ms;        /* when a waiting service may start again */
    int present;                /* seen by the last configuration read */
};

static struct entry entries[MAX_ENTRIES];
static int entry_count;
static char config_path[128] = CONFIG_PATH;
static volatile int shutdown_request;   /* the signal received, 0 if none */
static volatile int child_exited;
static int listener = -1;
static int connection = -1;     /* the initctl request being served, or -1 */

static const char *const kind_names[] = { "task", "service", "console" };
static const char *const state_names[] = {
    "waiting", "running", "stopping", "stopped", "done", "failed", "skipped"
};

static void on_signal(int sig)
{
    if (sig == SIGCHLD)
        child_exited = 1;
    else
        shutdown_request = sig;
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void report(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void report(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("init: ", stdout);
    vprintf(fmt, ap);
    putchar('\n');
    fflush(stdout);
    va_end(ap);
}

/* ---- configuration ---- */

static struct entry *find_entry(const char *name)
{
    for (int i = 0; i < entry_count; i++)
        if (strcmp(entries[i].name, name) == 0)
            return &entries[i];
    return NULL;
}

/* Fill the command and options of an entry from the words after the
 * name. Returns 0, or -1 with a message printed. */
static int set_command(struct entry *e, char **words, int count, int lineno)
{
    e->require[0] = e->log[0] = 0;
    e->restart = RESTART_ALWAYS;
    int i = 0;
    for (; i < count; i++) {
        if (strncmp(words[i], "if=", 3) == 0)
            strlcpy(e->require, words[i] + 3, sizeof e->require);
        else if (strncmp(words[i], "log=", 4) == 0)
            strlcpy(e->log, words[i] + 4, sizeof e->log);
        else if (strcmp(words[i], "restart=always") == 0)
            e->restart = RESTART_ALWAYS;
        else if (strcmp(words[i], "restart=never") == 0)
            e->restart = RESTART_NEVER;
        else if (strcmp(words[i], "restart=failure") == 0)
            e->restart = RESTART_FAILURE;
        else
            break;
    }
    if (i == count) {
        report("%s line %d: %s without a command", config_path, lineno, e->name);
        return -1;
    }
    if (count - i > MAX_ARGS) {
        report("%s line %d: too many arguments", config_path, lineno);
        return -1;
    }
    /* Copy the command words into the entry's own storage; the vector
     * is rebuilt from it at every start, so entries may be copied. */
    char *p = e->words;
    e->argc = 0;
    for (; i < count; i++) {
        size_t len = strlen(words[i]) + 1;
        if (p + len > e->words + sizeof e->words) {
            report("%s line %d: command too long", config_path, lineno);
            return -1;
        }
        memcpy(p, words[i], len);
        e->argc++;
        p += len;
    }
    return 0;
}

static void build_argv(const struct entry *e, char *argv[MAX_ARGS + 1])
{
    const char *p = e->words;
    for (int i = 0; i < e->argc; i++) {
        argv[i] = (char *)p;
        p += strlen(p) + 1;
    }
    argv[e->argc] = NULL;
}

/* Parse one configuration line. Entries are created or, when their
 * name exists, updated in place so that a reload keeps the runtime
 * state of the programs that keep running. */
static void parse_line(char *line, int lineno)
{
    char *hash = strchr(line, '#');
    if (hash)
        *hash = 0;
    char *words[MAX_ARGS + 8];
    int count = 0;
    const char *save;
    for (char *w = strtok_r(line, " \t\r\n", &save); w; w = strtok_r(NULL, " \t\r\n", &save)) {
        if (count == (int)(sizeof words / sizeof words[0])) {
            report("%s line %d: too many words", config_path, lineno);
            return;
        }
        words[count++] = w;
    }
    if (count == 0)
        return;
    if (strcmp(words[0], "env") == 0) {
        for (int i = 1; i < count; i++) {
            char *eq = strchr(words[i], '=');
            if (!eq) {
                report("%s line %d: env expects NAME=VALUE", config_path, lineno);
                continue;
            }
            *eq = 0;
            setenv(words[i], eq + 1, 1);
        }
        return;
    }
    enum kind kind;
    if (strcmp(words[0], "task") == 0)
        kind = KIND_TASK;
    else if (strcmp(words[0], "service") == 0)
        kind = KIND_SERVICE;
    else if (strcmp(words[0], "console") == 0)
        kind = KIND_CONSOLE;
    else {
        report("%s line %d: unknown keyword %s", config_path, lineno, words[0]);
        return;
    }
    if (count < 2) {
        report("%s line %d: %s without a name", config_path, lineno, words[0]);
        return;
    }
    struct entry *e = find_entry(words[1]);
    if (e && e->present) {
        report("%s line %d: duplicate entry %s", config_path, lineno, words[1]);
        return;
    }
    if (!e && entry_count == MAX_ENTRIES) {
        report("%s line %d: more than %d entries", config_path, lineno, MAX_ENTRIES);
        return;
    }
    /* Parse into a copy so that a bad line leaves an existing entry as
     * it was. */
    struct entry parsed;
    if (e)
        parsed = *e;
    else {
        memset(&parsed, 0, sizeof parsed);
        strlcpy(parsed.name, words[1], sizeof parsed.name);
        parsed.state = STATE_WAITING;
    }
    parsed.kind = kind;
    if (set_command(&parsed, words + 2, count - 2, lineno) < 0)
        return;
    parsed.present = 1;
    if (e)
        *e = parsed;
    else
        entries[entry_count++] = parsed;
}

static void builtin_config(void)
{
    static const char *const lines[] = {
        "env PATH=/bin HOME=/home USER=user SHELL=/bin/sh TERM=minios",
        "task fsinit fsinit",
        "task network net apply",
        "console sh sh",
    };
    for (size_t i = 0; i < sizeof lines / sizeof lines[0]; i++) {
        char copy[128];
        strlcpy(copy, lines[i], sizeof copy);
        parse_line(copy, (int)i + 1);
    }
}

/* Read the configuration. Entries that disappeared are marked absent
 * (present == 0) for the caller to stop. Returns 0 when the file was read. */
static int read_config(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        report("%s: %s", path, strerror(errno));
        return -1;
    }
    for (int i = 0; i < entry_count; i++)
        entries[i].present = 0;
    strlcpy(config_path, path, sizeof config_path);
    char line[512];
    int lineno = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        parse_line(line, lineno);
    }
    fclose(f);
    return 0;
}

/* ---- processes ---- */

static void start_entry(struct entry *e)
{
    struct stat st;
    if (e->require[0] && stat(e->require, &st) < 0) {
        e->state = STATE_SKIPPED;
        report("%s skipped, %s is absent", e->name, e->require);
        return;
    }
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        /* The control descriptors stay with init: a program holding the
         * connection would keep the client waiting for the end of the
         * reply. */
        if (listener >= 0)
            close(listener);
        if (connection >= 0)
            close(connection);
        if (e->kind == KIND_CONSOLE) {
            setpgid(0, 0);
            tcsetpgrp(0, getpid());
        }
        if (e->log[0]) {
            int fd = open(e->log, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (fd >= 0) {
                dup2(fd, 1);
                dup2(fd, 2);
                if (fd > 2)
                    close(fd);
            } else {
                fprintf(stderr, "init: %s: %s: %s\n", e->name, e->log, strerror(errno));
            }
        }
        char *argv[MAX_ARGS + 1];
        build_argv(e, argv);
        execvp(argv[0], argv);
        fprintf(stderr, "init: %s: exec %s: %s\n", e->name, argv[0], strerror(errno));
        _exit(127);
    }
    if (pid < 0) {
        e->state = STATE_FAILED;
        report("%s: fork: %s", e->name, strerror(errno));
        return;
    }
    if (e->kind == KIND_CONSOLE)
        setpgid(pid, pid);
    e->pid = pid;
    e->state = STATE_RUNNING;
    e->starts++;
    e->started_ms = now_ms();
}

static struct entry *entry_by_pid(pid_t pid)
{
    for (int i = 0; i < entry_count; i++)
        if (entries[i].pid == pid)
            return &entries[i];
    return NULL;
}

static void describe_status(int status, char *buf, size_t size)
{
    if (WIFSIGNALED(status))
        snprintf(buf, size, "signal %d", WTERMSIG(status));
    else
        snprintf(buf, size, "status %d", WEXITSTATUS(status));
}

/* The process of an entry has exited: record it and decide whether the
 * entry restarts. */
static void entry_exited(struct entry *e, int status)
{
    char how[32];
    describe_status(status, how, sizeof how);
    e->pid = 0;
    e->status = status;
    int failed = !WIFEXITED(status) || WEXITSTATUS(status) != 0;
    if (e->state == STATE_STOPPING) {
        e->state = STATE_STOPPED;
        report("%s stopped", e->name);
        return;
    }
    if (e->kind == KIND_TASK) {
        e->state = failed ? STATE_FAILED : STATE_DONE;
        if (failed)
            report("%s failed with %s", e->name, how);
        return;
    }
    uint64_t ran = now_ms() - e->started_ms;
    e->quick = ran < QUICK_MS ? e->quick + 1 : 0;
    int restart = e->restart == RESTART_ALWAYS || (e->restart == RESTART_FAILURE && failed);
    if (e->kind == KIND_CONSOLE)
        restart = 1;            /* the system is unusable without a session */
    if (!restart) {
        e->state = failed ? STATE_FAILED : STATE_STOPPED;
        report("%s exited with %s", e->name, how);
        return;
    }
    if (e->kind != KIND_CONSOLE && e->quick >= QUICK_LIMIT) {
        e->state = STATE_FAILED;
        report("%s exited with %s %d times within %d s, giving up", e->name, how, e->quick, QUICK_MS / 1000);
        return;
    }
    report("%s exited with %s, restarting", e->name, how);
    e->state = STATE_WAITING;
    e->restart_ms = now_ms() + (e->quick ? RESTART_MS : 0);
}

/* Collect every exited child. Children of exited programs were
 * reparented to init and are reaped here as well. */
static void reap(void)
{
    child_exited = 0;
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        struct entry *e = entry_by_pid(pid);
        if (e)
            entry_exited(e, status);
    }
}

/* Wait for one particular process, reaping the others meanwhile. */
static void wait_for(struct entry *e)
{
    while (e->pid > 0) {
        int status;
        pid_t pid = waitpid(-1, &status, 0);
        if (pid < 0) {
            if (errno != ECHILD)
                continue;   /* EINTR from a signal, handled by the caller */
            e->pid = 0;     /* cannot happen while the process exists */
            e->state = STATE_FAILED;
            break;
        }
        struct entry *x = entry_by_pid(pid);
        if (x)
            entry_exited(x, status);
    }
}

/* Stop a running entry: SIGTERM, then SIGKILL after the grace period.
 * Returns when the process has been reaped. */
static void stop_entry(struct entry *e)
{
    if (e->pid <= 0) {
        if (e->state == STATE_WAITING)
            e->state = STATE_STOPPED;
        return;
    }
    e->state = STATE_STOPPING;
    kill(e->kind == KIND_CONSOLE ? -e->pid : e->pid, SIGTERM);
    uint64_t deadline = now_ms() + STOP_MS;
    while (e->pid > 0 && now_ms() < deadline) {
        reap();
        if (e->pid > 0)
            sleep_ms(20);
    }
    if (e->pid > 0) {
        report("%s ignored SIGTERM, killing it", e->name);
        kill(e->kind == KIND_CONSOLE ? -e->pid : e->pid, SIGKILL);
        wait_for(e);
    }
}

/* Start what is due: tasks run to completion in order, the rest start
 * in the background. Called after every configuration read and from
 * the main loop for restarts whose delay has expired. */
static void start_due(void)
{
    uint64_t now = now_ms();
    for (int i = 0; i < entry_count; i++) {
        struct entry *e = &entries[i];
        if (e->state != STATE_WAITING || !e->present || e->restart_ms > now)
            continue;
        start_entry(e);
        if (e->kind == KIND_TASK && e->pid > 0)
            wait_for(e);
    }
}

/* Reload the configuration: entries that vanished are stopped, new ones
 * started, existing ones keep running with their new command taking
 * effect at the next start. */
static void reload(const char *path, char *reply, size_t size)
{
    if (read_config(path) < 0) {
        snprintf(reply, size, "error: cannot read %s\n", path);
        return;
    }
    int stopped = 0, kept = 0;
    for (int i = 0; i < entry_count; i++) {
        struct entry *e = &entries[i];
        if (!e->present) {
            if (e->pid > 0) {
                stop_entry(e);
                stopped++;
            }
            report("%s removed", e->name);
            memmove(e, e + 1, (size_t)(entry_count - i - 1) * sizeof *e);
            entry_count--;
            i--;
        } else {
            kept++;
        }
    }
    start_due();
    snprintf(reply, size, "ok\n%d entries, %d stopped\n", kept, stopped);
}

/* ---- shutdown ---- */

static void shutdown_system(int cmd)
{
    report("stopping services");
    for (int i = entry_count - 1; i >= 0; i--) {
        struct entry *e = &entries[i];
        if (e->pid > 0) {
            e->state = STATE_STOPPING;
            kill(e->kind == KIND_CONSOLE ? -e->pid : e->pid, SIGTERM);
        }
    }
    uint64_t deadline = now_ms() + STOP_MS;
    for (;;) {
        reap();
        int running = 0;
        for (int i = 0; i < entry_count; i++)
            running += entries[i].pid > 0;
        if (!running || now_ms() >= deadline)
            break;
        sleep_ms(20);
    }
    report("%s", cmd == RB_AUTOBOOT ? "rebooting" : cmd == RB_HALT ? "halting" : "powering off");
    /* The kernel terminates whatever is left, flushes and unmounts. */
    reboot(cmd);
    report("reboot: %s", strerror(errno));
}

/* ---- control socket ---- */

static void append(char *reply, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void append(char *reply, size_t size, const char *fmt, ...)
{
    size_t used = strlen(reply);
    if (used >= size - 1)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(reply + used, size - used, fmt, ap);
    va_end(ap);
}

static void format_entry(const struct entry *e, char *reply, size_t size)
{
    char pid[16] = "-", detail[48] = "";
    if (e->pid > 0)
        snprintf(pid, sizeof pid, "%d", (int)e->pid);
    if (e->state == STATE_FAILED && e->starts > 0)
        describe_status(e->status, detail, sizeof detail);
    else if (e->state == STATE_SKIPPED)
        snprintf(detail, sizeof detail, "%s absent", e->require);
    else if (e->state == STATE_RUNNING) {
        uint64_t s = (now_ms() - e->started_ms) / 1000;
        snprintf(detail, sizeof detail, "%lu:%02lu", (unsigned long)s / 60, (unsigned long)s % 60);
    }
    append(reply, size, "%-12s %-8s %-9s %6s %6d  %s\n", e->name, kind_names[e->kind],
           state_names[e->state], pid, e->starts, detail);
}

static void list_entries(char *reply, size_t size)
{
    append(reply, size, "%-12s %-8s %-9s %6s %6s  %s\n", "NAME", "KIND", "STATE", "PID", "STARTS", "");
    for (int i = 0; i < entry_count; i++)
        format_entry(&entries[i], reply, size);
}

static void handle_request(char *request, char *reply, size_t size)
{
    const char *save;
    char *cmd = strtok_r(request, " \t\r\n", &save);
    char *arg = strtok_r(NULL, " \t\r\n", &save);
    reply[0] = 0;
    if (!cmd || strcmp(cmd, "list") == 0) {
        append(reply, size, "ok\n");
        list_entries(reply, size);
        return;
    }
    if (strcmp(cmd, "reload") == 0) {
        reload(arg ? arg : config_path, reply, size);
        return;
    }
    if (strcmp(cmd, "poweroff") == 0 || strcmp(cmd, "reboot") == 0 || strcmp(cmd, "halt") == 0) {
        shutdown_request = cmd[0] == 'p' ? SIGUSR1 : cmd[0] == 'r' ? SIGUSR2 : SIGHUP;
        snprintf(reply, size, "ok\n");
        return;
    }
    int is_status = strcmp(cmd, "status") == 0, is_start = strcmp(cmd, "start") == 0;
    int is_stop = strcmp(cmd, "stop") == 0, is_restart = strcmp(cmd, "restart") == 0;
    if (!is_status && !is_start && !is_stop && !is_restart) {
        snprintf(reply, size, "error: %s: unknown command\n", cmd);
        return;
    }
    if (!arg) {
        snprintf(reply, size, "error: %s needs a name\n", cmd);
        return;
    }
    struct entry *e = find_entry(arg);
    if (!e) {
        snprintf(reply, size, "error: %s: no such entry\n", arg);
        return;
    }
    if (is_stop || is_restart) {
        if (e->kind == KIND_CONSOLE && is_stop) {
            snprintf(reply, size, "error: %s: the console session is restarted, use restart\n", arg);
            return;
        }
        if (e->pid > 0) {
            report("%s stopped by request", e->name);
            stop_entry(e);
        } else {
            e->state = STATE_STOPPED;
        }
    }
    if (is_start || is_restart) {
        if (e->pid > 0) {
            snprintf(reply, size, "error: %s is running as pid %d\n", arg, (int)e->pid);
            return;
        }
        e->quick = 0;
        e->state = STATE_WAITING;
        e->restart_ms = 0;
        report("%s started by request", e->name);
        start_entry(e);
        if (e->kind == KIND_TASK && e->pid > 0)
            wait_for(e);
    }
    append(reply, size, "ok\n");
    format_entry(e, reply, size);
}

static void serve_connection(int fd)
{
    connection = fd;
    char request[256];
    size_t got = 0;
    while (got < sizeof request - 1) {
        ssize_t n = read(fd, request + got, sizeof request - 1 - got);
        if (n <= 0)
            break;
        got += (size_t)n;
        if (memchr(request, '\n', got))
            break;
    }
    request[got] = 0;
    static char reply[REPLY_MAX];
    handle_request(request, reply, sizeof reply);
    size_t len = strlen(reply), done = 0;
    while (done < len) {
        ssize_t n = write(fd, reply + done, len - done);
        if (n <= 0)
            break;
        done += (size_t)n;
    }
    close(fd);
    connection = -1;
}

static int open_control(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_un addr = { AF_UNIX, CONTROL_NAME };
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(fd, 4) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int main(int argc, char **argv)
{
    /* Default actions never terminate init, so SIGINT and SIGTERM need no
     * handling here; ignoring them would be inherited by the shell. */
    signal(SIGUSR1, on_signal);
    signal(SIGUSR2, on_signal);
    signal(SIGHUP, on_signal);
    signal(SIGCHLD, on_signal);
    listener = open_control();
    if (listener < 0)
        report("control socket: %s", strerror(errno));
    if (read_config(argc > 1 ? argv[1] : CONFIG_PATH) < 0) {
        report("using the built-in configuration");
        builtin_config();
    }
    start_due();
    for (;;) {
        if (shutdown_request) {
            int sig = shutdown_request;
            shutdown_request = 0;
            shutdown_system(sig == SIGUSR2 ? RB_AUTOBOOT : sig == SIGHUP ? RB_HALT : RB_POWER_OFF);
        }
        reap();
        start_due();
        /* Sleep until a request, a child exit (SIGCHLD interrupts the
         * poll) or the next restart is due. The one second ceiling
         * covers a SIGCHLD that arrives between reap() and poll(). */
        uint64_t now = now_ms();
        int timeout = 1000;
        for (int i = 0; i < entry_count; i++) {
            struct entry *e = &entries[i];
            if (e->state == STATE_WAITING && e->present && e->restart_ms > now && e->restart_ms - now < (uint64_t)timeout)
                timeout = (int)(e->restart_ms - now);
        }
        if (listener < 0) {
            sleep_ms((unsigned long)timeout);
            continue;
        }
        struct pollfd pfd = { listener, POLLIN, 0 };
        int n = poll(&pfd, 1, timeout);
        if (n > 0 && (pfd.revents & POLLIN)) {
            int fd = accept(listener, NULL, NULL);
            if (fd >= 0)
                serve_connection(fd);
        }
    }
}
