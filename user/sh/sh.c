/* sh: the shell. Words are split with quoting ('...', "...", \), then
 * expanded ($NAME, ${NAME}, $?, $$, $#, $0..$9, $@). Command lists
 * combine pipelines with ;, &&, || and &. Redirections <, >, >> apply
 * per command. Builtins include POSIX-style jobs, fg and bg. Background
 * jobs are reaped and state changes are collected before each prompt. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/resource.h>

#define MAX_ARGS 64
#define MAX_CMDS 8
#define MAX_JOBS 16
#define WORD_MAX 1024

enum tok_kind { T_WORD, T_PIPE, T_LT, T_GT, T_GTGT, T_AMP, T_SEMI, T_AND, T_OR, T_END };

struct cmd {
    char *argv[MAX_ARGS];
    int argc;
    char *in, *out;
    int append;
};

struct job {
    int used;
    int state;
    pid_t pgid;
    pid_t pids[MAX_CMDS];
    int npids;
    char text[64];
};

enum { JOB_RUNNING, JOB_STOPPED };

static int interactive;
static int last_status;
static int script_argc;
static char **script_argv;
static struct job jobs[MAX_JOBS];

/* ---- expansion ---- */

static const char *var_lookup(const char *name, char *tmp, size_t size)
{
    if (strcmp(name, "?") == 0) {
        snprintf(tmp, size, "%d", last_status);
        return tmp;
    }
    if (strcmp(name, "$") == 0) {
        snprintf(tmp, size, "%d", getpid());
        return tmp;
    }
    if (strcmp(name, "#") == 0) {
        snprintf(tmp, size, "%d", script_argc > 0 ? script_argc - 1 : 0);
        return tmp;
    }
    if (strcmp(name, "@") == 0 || strcmp(name, "*") == 0) {
        tmp[0] = '\0';
        for (int i = 1; i < script_argc; i++) {
            if (i > 1)
                strlcat(tmp, " ", size);
            strlcat(tmp, script_argv[i], size);
        }
        return tmp;
    }
    if (name[0] >= '0' && name[0] <= '9' && !name[1]) {
        int i = name[0] - '0';
        return i < script_argc ? script_argv[i] : "";
    }
    const char *v = getenv(name);
    return v ? v : "";
}

static int run_line(const char *line);

/* Run cmd in a child with its standard output captured. Trailing newlines
 * are removed. Returns the number of bytes stored. */
static size_t capture(const char *cmd, char *out, size_t size)
{
    int fds[2];
    if (pipe(fds) < 0)
        return 0;
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return 0;
    }
    if (pid == 0) {
        dup2(fds[1], 1);
        close(fds[0]);
        close(fds[1]);
        interactive = 0;
        exit(run_line(cmd));
    }
    close(fds[1]);
    size_t n = 0;
    for (;;) {
        char buf[256];
        long r = read(fds[0], buf, sizeof buf);
        if (r <= 0)
            break;
        for (long i = 0; i < r && n < size - 1; i++)
            out[n++] = buf[i];
    }
    close(fds[0]);
    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    while (n > 0 && out[n - 1] == '\n')
        n--;
    out[n] = '\0';
    return n;
}

/* Append the expansion of a $ reference starting at *p (after the $). */
static void expand_var(const char **pp, char *out, size_t *len, size_t size)
{
    const char *p = *pp;
    char name[64];
    size_t n = 0;
    if (*p == '(') {
        /* Command substitution: find the matching parenthesis. */
        const char *start = ++p;
        int depth = 1;
        while (*p && depth) {
            if (*p == '(')
                depth++;
            else if (*p == ')')
                depth--;
            if (depth)
                p++;
        }
        static char cmd[WORD_MAX], result[WORD_MAX];
        snprintf(cmd, sizeof cmd, "%.*s", (int)(p - start), start);
        if (*p == ')')
            p++;
        *pp = p;
        capture(cmd, result, sizeof result);
        for (const char *v = result; *v && *len < size - 1; v++)
            out[(*len)++] = *v;
        return;
    }
    if (*p == '{') {
        p++;
        while (*p && *p != '}' && n < sizeof name - 1)
            name[n++] = *p++;
        if (*p == '}')
            p++;
    } else if (*p == '?' || *p == '$' || *p == '#' || *p == '@' || *p == '*' || (*p >= '0' && *p <= '9')) {
        name[n++] = *p++;
    } else {
        while ((*p == '_' || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9')) &&
               n < sizeof name - 1)
            name[n++] = *p++;
    }
    name[n] = '\0';
    *pp = p;
    if (n == 0) {
        if (*len < size - 1)
            out[(*len)++] = '$';
        return;
    }
    char tmp[256];
    const char *v = var_lookup(name, tmp, sizeof tmp);
    while (*v && *len < size - 1)
        out[(*len)++] = *v++;
}

/* ---- tokenizer ---- */

struct lexer {
    const char *p;
    char word[WORD_MAX];
};

static enum tok_kind next_token(struct lexer *lx)
{
    const char *p = lx->p;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p == '#') {
        while (*p && *p != '\n')
            p++;
    }
    if (!*p || *p == '\n') {
        lx->p = p;
        return T_END;
    }
    if (*p == '|') {
        lx->p = p + (p[1] == '|' ? 2 : 1);
        return p[1] == '|' ? T_OR : T_PIPE;
    }
    if (*p == '&') {
        lx->p = p + (p[1] == '&' ? 2 : 1);
        return p[1] == '&' ? T_AND : T_AMP;
    }
    if (*p == ';') {
        lx->p = p + 1;
        return T_SEMI;
    }
    if (*p == '<') {
        lx->p = p + 1;
        return T_LT;
    }
    if (*p == '>') {
        lx->p = p + (p[1] == '>' ? 2 : 1);
        return p[1] == '>' ? T_GTGT : T_GT;
    }
    size_t len = 0;
    while (*p && !strchr(" \t\n|&;<>", *p)) {
        if (*p == '\'') {
            p++;
            while (*p && *p != '\'' && len < WORD_MAX - 1)
                lx->word[len++] = *p++;
            if (*p == '\'')
                p++;
        } else if (*p == '"') {
            p++;
            while (*p && *p != '"' && len < WORD_MAX - 1) {
                if (*p == '\\' && (p[1] == '"' || p[1] == '\\' || p[1] == '$')) {
                    lx->word[len++] = p[1];
                    p += 2;
                } else if (*p == '$') {
                    p++;
                    expand_var(&p, lx->word, &len, WORD_MAX);
                } else {
                    lx->word[len++] = *p++;
                }
            }
            if (*p == '"')
                p++;
        } else if (*p == '\\' && p[1]) {
            lx->word[len++] = p[1];
            p += 2;
        } else if (*p == '$') {
            p++;
            expand_var(&p, lx->word, &len, WORD_MAX);
        } else {
            lx->word[len++] = *p++;
        }
    }
    lx->word[len] = '\0';
    lx->p = p;
    return T_WORD;
}

/* ---- jobs ---- */

static int job_alive(const struct job *j)
{
    for (int i = 0; i < j->npids; i++)
        if (j->pids[i] > 0)
            return 1;
    return 0;
}

static int job_add(pid_t pgid, pid_t *pids, int n, const char *text,
                   int state, int announce)
{
    for (int i = 0; i < MAX_JOBS; i++) {
        if (!jobs[i].used) {
            jobs[i].used = 1;
            jobs[i].state = state;
            jobs[i].pgid = pgid;
            jobs[i].npids = n;
            memcpy(jobs[i].pids, pids, (size_t)n * sizeof pids[0]);
            strlcpy(jobs[i].text, text, sizeof jobs[i].text);
            if (announce) {
                if (state == JOB_STOPPED)
                    printf("[%d] Stopped  %s\n", i + 1, jobs[i].text);
                else
                    printf("[%d] %d\n", i + 1, pgid);
            }
            return i;
        }
    }
    fprintf(stderr, "sh: too many jobs\n");
    return -1;
}

static void job_event(struct job *j, pid_t pid, int status)
{
    if (WIFSTOPPED(status)) {
        j->state = JOB_STOPPED;
        return;
    }
    if (WIFCONTINUED(status)) {
        j->state = JOB_RUNNING;
        return;
    }
    for (int i = 0; i < j->npids; i++)
        if (j->pids[i] == pid)
            j->pids[i] = 0;
}

static void job_drain(struct job *j, int block)
{
    for (;;) {
        if (!job_alive(j) || (block && j->state == JOB_STOPPED))
            return;
        int status;
        int options = WUNTRACED | WCONTINUED | (block ? 0 : WNOHANG);
        pid_t r = waitpid(-j->pgid, &status, options);
        if (r > 0) {
            job_event(j, r, status);
            continue;
        }
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0 && errno == ECHILD)
            memset(j->pids, 0, sizeof j->pids);
        return;
    }
}

/* Reap finished background processes; with block, wait for running jobs. */
static void jobs_reap(int block)
{
    for (int i = 0; i < MAX_JOBS; i++) {
        if (!jobs[i].used)
            continue;
        job_drain(&jobs[i], block);
        if (!job_alive(&jobs[i])) {
            printf("[%d] Done  %s\n", i + 1, jobs[i].text);
            jobs[i].used = 0;
        }
    }
}

static int job_select(const char *arg)
{
    if (!arg || strcmp(arg, "%+") == 0 || strcmp(arg, "%%") == 0) {
        for (int i = MAX_JOBS - 1; i >= 0; i--)
            if (jobs[i].used)
                return i;
        return -1;
    }
    if (*arg == '%')
        arg++;
    char *end;
    long n = strtol(arg, &end, 10);
    if (*arg && !*end && n >= 1 && n <= MAX_JOBS && jobs[n - 1].used)
        return (int)n - 1;
    return -1;
}

static int wait_foreground(struct job *j)
{
    int result = 0;
    pid_t last = j->pids[j->npids - 1];
    while (job_alive(j)) {
        int status;
        pid_t r = waitpid(-j->pgid, &status, WUNTRACED);
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0) {
            if (errno == ECHILD)
                memset(j->pids, 0, sizeof j->pids);
            break;
        }
        job_event(j, r, status);
        if (WIFSTOPPED(status)) {
            result = 128 + WSTOPSIG(status);
            job_drain(j, 0);
            break;
        }
        if (r == last) {
            if (WIFSIGNALED(status)) {
                int sig = WTERMSIG(status);
                if (sig != SIGPIPE)
                    printf("[%s terminated by signal %d]\n", j->text, sig);
                result = 128 + sig;
            } else if (WIFEXITED(status)) {
                result = WEXITSTATUS(status);
            }
        }
    }
    if (!job_alive(j))
        j->used = 0;
    return result;
}

static int builtin_bg(const char *arg)
{
    jobs_reap(0);
    int i = job_select(arg);
    if (i < 0) {
        fprintf(stderr, "bg: no such job\n");
        return 1;
    }
    if (kill(-jobs[i].pgid, SIGCONT) < 0) {
        fprintf(stderr, "bg: %s\n", strerror(errno));
        return 1;
    }
    jobs[i].state = JOB_RUNNING;
    printf("[%d] %d  %s\n", i + 1, jobs[i].pgid, jobs[i].text);
    return 0;
}

static int builtin_fg(const char *arg)
{
    jobs_reap(0);
    int i = job_select(arg);
    if (i < 0) {
        fprintf(stderr, "fg: no such job\n");
        return 1;
    }
    printf("%s\n", jobs[i].text);
    if (interactive)
        tcsetpgrp(0, jobs[i].pgid);
    if (jobs[i].state == JOB_STOPPED && kill(-jobs[i].pgid, SIGCONT) < 0) {
        if (interactive)
            tcsetpgrp(0, getpgrp());
        fprintf(stderr, "fg: %s\n", strerror(errno));
        return 1;
    }
    jobs[i].state = JOB_RUNNING;
    int status = wait_foreground(&jobs[i]);
    if (interactive)
        tcsetpgrp(0, getpgrp());
    if (jobs[i].used && jobs[i].state == JOB_STOPPED)
        printf("[%d] Stopped  %s\n", i + 1, jobs[i].text);
    return status;
}

/* ---- builtins ---- */

/* ulimit [-HS] [-acdfnstuv] [value]: the resource limits of the shell,
 * inherited by every command it starts. Sizes are in KiB, -f in 512 byte
 * blocks, -t in seconds, -n and -u are counts. */
struct ulimit_opt {
    char letter;
    int resource;
    unsigned long unit;
    const char *desc;
};
static const struct ulimit_opt ulimit_opts[] = {
    { 'c', RLIMIT_CORE, 1024, "core file size (KiB)" },
    { 'd', RLIMIT_DATA, 1024, "data seg size (KiB)" },
    { 'f', RLIMIT_FSIZE, 512, "file size (blocks)" },
    { 'n', RLIMIT_NOFILE, 1, "open files" },
    { 's', RLIMIT_STACK, 1024, "stack size (KiB)" },
    { 't', RLIMIT_CPU, 1, "cpu time (seconds)" },
    { 'u', RLIMIT_NPROC, 1, "max user processes" },
    { 'v', RLIMIT_AS, 1024, "virtual memory (KiB)" },
};

static void ulimit_print(const struct ulimit_opt *o, int hard, int with_desc)
{
    struct rlimit rl;
    if (getrlimit(o->resource, &rl) < 0) {
        fprintf(stderr, "ulimit: %s\n", strerror(errno));
        return;
    }
    unsigned long v = hard ? rl.rlim_max : rl.rlim_cur;
    if (with_desc)
        printf("%-24s (-%c) ", o->desc, o->letter);
    if (v == RLIM_INFINITY)
        printf("unlimited\n");
    else
        printf("%lu\n", v / o->unit);
}

static int builtin_ulimit(char **argv)
{
    int hard = 0, soft = 0, all = 0;
    const struct ulimit_opt *sel = NULL;
    int i = 1;
    for (; argv[i] && argv[i][0] == '-' && argv[i][1]; i++) {
        for (const char *c = argv[i] + 1; *c; c++) {
            if (*c == 'H') { hard = 1; continue; }
            if (*c == 'S') { soft = 1; continue; }
            if (*c == 'a') { all = 1; continue; }
            const struct ulimit_opt *o = NULL;
            for (size_t k = 0; k < sizeof ulimit_opts / sizeof ulimit_opts[0]; k++)
                if (ulimit_opts[k].letter == *c)
                    o = &ulimit_opts[k];
            if (!o) {
                fprintf(stderr, "ulimit: -%c: invalid option\n", *c);
                fprintf(stderr, "usage: ulimit [-HS] [-acdfnstuv] [value|unlimited]\n");
                return 2;
            }
            sel = o;
        }
    }
    if (all) {
        for (size_t k = 0; k < sizeof ulimit_opts / sizeof ulimit_opts[0]; k++)
            ulimit_print(&ulimit_opts[k], hard, 1);
        return 0;
    }
    if (!sel)
        sel = &ulimit_opts[2];              /* -f, as in other shells */
    if (!argv[i]) {
        ulimit_print(sel, hard, 0);
        return 0;
    }
    struct rlimit rl;
    if (getrlimit(sel->resource, &rl) < 0) {
        fprintf(stderr, "ulimit: %s\n", strerror(errno));
        return 1;
    }
    unsigned long v;
    if (strcmp(argv[i], "unlimited") == 0) {
        v = RLIM_INFINITY;
    } else {
        char *end;
        v = strtoul(argv[i], &end, 10);
        if (*end || end == argv[i]) {
            fprintf(stderr, "ulimit: %s: invalid number\n", argv[i]);
            return 1;
        }
        v *= sel->unit;
    }
    if (!hard && !soft)
        hard = soft = 1;                    /* both, like other shells */
    if (hard)
        rl.rlim_max = v;
    if (soft)
        rl.rlim_cur = v;
    if (rl.rlim_cur > rl.rlim_max)
        rl.rlim_cur = rl.rlim_max;
    if (setrlimit(sel->resource, &rl) < 0) {
        fprintf(stderr, "ulimit: %s\n", strerror(errno));
        return 1;
    }
    return 0;
}

static int builtin(struct cmd *c)
{
    char **argv = c->argv;
    if (strcmp(argv[0], "exit") == 0) {
        jobs_reap(0);
        exit(argv[1] ? atoi(argv[1]) : last_status);
    }
    if (strcmp(argv[0], "cd") == 0) {
        const char *dir = argv[1] ? argv[1] : "/";
        if (chdir(dir) < 0) {
            fprintf(stderr, "cd: %s: %s\n", dir, strerror(errno));
            return 1;
        }
        return 0;
    }
    if (strcmp(argv[0], "pwd") == 0) {
        char buf[256];
        if (getcwd(buf, sizeof buf))
            printf("%s\n", buf);
        return 0;
    }
    if (strcmp(argv[0], "export") == 0) {
        for (int i = 1; argv[i]; i++) {
            char *eq = strchr(argv[i], '=');
            if (eq) {
                *eq = '\0';
                setenv(argv[i], eq + 1, 1);
            } else if (!getenv(argv[i])) {
                setenv(argv[i], "", 1);
            }
        }
        return 0;
    }
    if (strcmp(argv[0], "unset") == 0) {
        for (int i = 1; argv[i]; i++)
            unsetenv(argv[i]);
        return 0;
    }
    if (strcmp(argv[0], "set") == 0) {
        for (char **e = environ; *e; e++)
            printf("%s\n", *e);
        return 0;
    }
    if (strcmp(argv[0], "jobs") == 0) {
        jobs_reap(0);
        for (int i = 0; i < MAX_JOBS; i++)
            if (jobs[i].used)
                printf("[%d] %s  %s\n", i + 1,
                       jobs[i].state == JOB_STOPPED ? "Stopped" : "Running",
                       jobs[i].text);
        return 0;
    }
    if (strcmp(argv[0], "bg") == 0)
        return builtin_bg(argv[1]);
    if (strcmp(argv[0], "fg") == 0)
        return builtin_fg(argv[1]);
    if (strcmp(argv[0], "wait") == 0) {
        jobs_reap(1);
        return 0;
    }
    if (strcmp(argv[0], "true") == 0)
        return 0;
    if (strcmp(argv[0], "false") == 0)
        return 1;
    if (strcmp(argv[0], "ulimit") == 0)
        return builtin_ulimit(argv);
    if (strcmp(argv[0], "help") == 0) {
        printf("builtins: cd exit pwd export unset set jobs fg bg wait true false ulimit help\n");
        printf("quoting: '...' \"...\" ; variables: $NAME ${NAME} $? $$ $# $0-$9 $@\n");
        printf("lists: ; && || &; pipelines: |; redirections: < > >>\n");
        return 0;
    }
    return -1;
}

/* ---- execution ---- */

static void exec_child(struct cmd *c)
{
    signal(SIGINT, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGTTIN, SIG_DFL);
    signal(SIGTTOU, SIG_DFL);
    if (c->in) {
        int fd = open(c->in, O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "sh: %s: %s\n", c->in, strerror(errno));
            _exit(1);
        }
        dup2(fd, 0);
        close(fd);
    }
    if (c->out) {
        int flags = O_WRONLY | O_CREAT | (c->append ? O_APPEND : O_TRUNC);
        int fd = open(c->out, flags, 0644);
        if (fd < 0) {
            fprintf(stderr, "sh: %s: %s\n", c->out, strerror(errno));
            _exit(1);
        }
        dup2(fd, 1);
        close(fd);
    }
    int r = builtin(c);
    if (r >= 0)
        _exit(r);
    execvp(c->argv[0], c->argv);
    fprintf(stderr, "%s: %s\n", c->argv[0], strerror(errno));
    _exit(127);
}

/* Run a pipeline; background pipelines are recorded as jobs. */
static int run_pipeline(struct cmd *cmds, int n, int background, const char *text)
{
    if (n == 1 && !background && !cmds[0].in && !cmds[0].out) {
        int r = builtin(&cmds[0]);
        if (r >= 0)
            return r;
    }
    pid_t pids[MAX_CMDS];
    pid_t pgid = 0;
    int prev_rd = -1;
    for (int i = 0; i < n; i++) {
        int fds[2] = { -1, -1 };
        if (i + 1 < n && pipe(fds) < 0) {
            perror("pipe");
            return 1;
        }
        pids[i] = fork();
        if (pids[i] < 0) {
            perror("fork");
            return 1;
        }
        if (pids[i] == 0) {
            setpgid(0, pgid);
            if (interactive && !background)
                tcsetpgrp(0, pgid ? pgid : getpid());
            if (prev_rd >= 0) {
                dup2(prev_rd, 0);
                close(prev_rd);
            }
            if (fds[1] >= 0) {
                dup2(fds[1], 1);
                close(fds[0]);
                close(fds[1]);
            }
            if (background && !interactive && !cmds[i].in && i == 0) {
                int null = open("/dev/null", O_RDONLY);
                if (null >= 0) {
                    dup2(null, 0);
                    close(null);
                }
            }
            exec_child(&cmds[i]);
        }
        if (!pgid)
            pgid = pids[i];
        setpgid(pids[i], pgid);
        if (interactive && !background)
            tcsetpgrp(0, pgid);
        if (prev_rd >= 0)
            close(prev_rd);
        if (fds[1] >= 0)
            close(fds[1]);
        prev_rd = fds[0];
    }
    if (background) {
        job_add(pgid, pids, n, text, JOB_RUNNING, 1);
        return 0;
    }
    struct job foreground;
    memset(&foreground, 0, sizeof foreground);
    foreground.used = 1;
    foreground.state = JOB_RUNNING;
    foreground.pgid = pgid;
    foreground.npids = n;
    memcpy(foreground.pids, pids, (size_t)n * sizeof pids[0]);
    strlcpy(foreground.text, text, sizeof foreground.text);
    int result = wait_foreground(&foreground);
    if (interactive)
        tcsetpgrp(0, getpgrp());
    if (foreground.used && foreground.state == JOB_STOPPED)
        job_add(pgid, foreground.pids, n, text, JOB_STOPPED, 1);
    return result;
}

/* Parse and run one line. Returns the status of the last command. */
static int run_line(const char *line)
{
    struct lexer lx = { .p = line };
    static char words[MAX_CMDS * MAX_ARGS][128];
    int nwords = 0;
    struct cmd cmds[MAX_CMDS];
    int ncmd = 0;
    memset(&cmds[0], 0, sizeof cmds[0]);
    enum tok_kind pending_op = T_SEMI;  /* how the previous list item ended */
    const char *start = line;

    for (;;) {
        enum tok_kind t = next_token(&lx);
        struct cmd *c = &cmds[ncmd];
        if (t == T_WORD || t == T_LT || t == T_GT || t == T_GTGT) {
            if (t != T_WORD) {
                enum tok_kind r = t;
                if (next_token(&lx) != T_WORD) {
                    fprintf(stderr, "sh: missing file name after redirection\n");
                    return 2;
                }
                if (nwords >= MAX_CMDS * MAX_ARGS)
                    return 2;
                strlcpy(words[nwords], lx.word, sizeof words[0]);
                if (r == T_LT) {
                    c->in = words[nwords];
                } else {
                    c->out = words[nwords];
                    c->append = r == T_GTGT;
                }
                nwords++;
                continue;
            }
            if (c->argc >= MAX_ARGS - 1 || nwords >= MAX_CMDS * MAX_ARGS) {
                fprintf(stderr, "sh: too many words\n");
                return 2;
            }
            strlcpy(words[nwords], lx.word, sizeof words[0]);
            c->argv[c->argc++] = words[nwords++];
            continue;
        }
        if (t == T_PIPE) {
            if (c->argc == 0 || ncmd + 1 >= MAX_CMDS) {
                fprintf(stderr, "sh: syntax error near |\n");
                return 2;
            }
            memset(&cmds[++ncmd], 0, sizeof cmds[0]);
            continue;
        }
        /* End of a pipeline: ; & && || or end of line. */
        if (c->argc == 0) {
            if (ncmd > 0) {
                fprintf(stderr, "sh: syntax error near |\n");
                return 2;
            }
            if (t == T_END)
                break;
            continue;
        }
        int run = 1;
        if (pending_op == T_AND && last_status != 0)
            run = 0;
        if (pending_op == T_OR && last_status == 0)
            run = 0;
        if (run) {
            /* A lone NAME=value word assigns a variable of this shell. */
            char *eq = strchr(c->argv[0], '=');
            int is_name = eq && eq != c->argv[0];
            for (const char *q = c->argv[0]; is_name && q < eq; q++)
                if (!(*q == '_' || (*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9')))
                    is_name = 0;
            if (ncmd == 0 && c->argc == 1 && is_name) {
                *eq = '\0';
                setenv(c->argv[0], eq + 1, 1);
                last_status = 0;
                c->argc = 0;
            }
            if (c->argc > 0) {
                char text[64];
                size_t n = (size_t)(lx.p - start);
                strlcpy(text, start, n + 1 < sizeof text ? n + 1 : sizeof text);
                last_status = run_pipeline(cmds, ncmd + 1, t == T_AMP, text);
            }
        }
        /* && and || are evaluated against last_status on the next item. */
        pending_op = t;
        ncmd = 0;
        memset(&cmds[0], 0, sizeof cmds[0]);
        start = lx.p;
        if (t == T_END)
            break;
    }
    return last_status;
}

int main(int argc, char **argv)
{
    char line[1024];
    if (argc > 2 && strcmp(argv[1], "-c") == 0) {
        script_argc = argc - 2;
        script_argv = argv + 2;
        return run_line(argv[2]);
    }
    FILE *in = stdin;
    interactive = 1;
    if (argc > 1) {
        in = fopen(argv[1], "r");
        if (!in) {
            fprintf(stderr, "sh: %s: %s\n", argv[1], strerror(errno));
            return 1;
        }
        interactive = 0;
        script_argc = argc - 1;
        script_argv = argv + 1;
    } else {
        script_argc = 1;
        script_argv = argv;
    }
    if (interactive) {
        signal(SIGINT, SIG_IGN);
        signal(SIGPIPE, SIG_IGN);
        signal(SIGTSTP, SIG_IGN);
        signal(SIGTTIN, SIG_IGN);
        signal(SIGTTOU, SIG_IGN);
        setpgid(0, 0);
        tcsetpgrp(0, getpgrp());
    }
    for (;;) {
        if (interactive) {
            jobs_reap(0);
            char cwd[256];
            printf("%s $ ", getcwd(cwd, sizeof cwd) ? cwd : "?");
            fflush(stdout);
        }
        if (!fgets(line, sizeof line, in)) {
            if (interactive)
                printf("\n");
            jobs_reap(0);
            return last_status;
        }
        run_line(line);
    }
}
