#include "sh.h"
#include <fnmatch.h>

static int in_child;
static int alias_depth;
static const char *command_text;
static int execute_one(struct node *n);

static int wait_child(pid_t pid)
{
    int status;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            return 1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

void child_signals(void)
{
    signal(SIGINT, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGTTIN, SIG_DFL);
    signal(SIGTTOU, SIG_DFL);
}

int command_run(int argc, char **argv, int allow_functions)
{
    if (!argc)
        return 0;
    struct node *function = allow_functions ? function_get(argv[0]) : NULL;
    if (function) {
        if (function_depth >= 64) {
            fprintf(stderr, "sh: function recursion limit\n");
            return 2;
        }
        /* Copy before executing: a function may redefine itself. */
        struct node *body = node_clone(function);
        struct parameters previous = parameters_push(argc, argv);
        int old_child = in_child;
        in_child = 0;
        scope_push();
        function_depth++;
        int result = exec_node(body);
        function_depth--;
        scope_pop();
        parameters_pop(previous);
        in_child = old_child;
        if (flow == FLOW_RETURN)
            flow = FLOW_NORMAL;
        node_free(body);
        return result;
    }
    int result = builtin(argc, argv);
    if (result >= 0)
        return result;

    if (in_child) {
        execvp(argv[0], argv);
        fprintf(stderr, "%s: %s\n", argv[0], strerror(errno));
        return errno == ENOENT ? 127 : 126;
    }
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }
    if (!pid) {
        child_signals();
        setpgid(0, 0);
        if (interactive)
            tcsetpgrp(0, getpid());
        execvp(argv[0], argv);
        fprintf(stderr, "%s: %s\n", argv[0], strerror(errno));
        _exit(errno == ENOENT ? 127 : 126);
    }
    setpgid(pid, pid);
    if (interactive)
        tcsetpgrp(0, pid);
    struct job job = {.used = 1, .state = JOB_RUNNING, .pgid = pid, .npids = 1};
    job.pids[0] = pid;
    snprintf(job.text, sizeof job.text, "%s", command_text ? command_text : argv[0]);
    result = wait_foreground(&job);
    if (interactive)
        tcsetpgrp(0, getpgrp());
    if (job.used && job.state == JOB_STOPPED)
        job_add(pid, job.pids, job.statuses, 1, job.text, JOB_STOPPED, 1);
    return result;
}

static int simple(struct node *n)
{
    unsigned long substitutions_before = substitutions;
    struct word *first = n->words;
    while (first && assignment(first->text))
        first = first->next;

    int temporary = first != NULL && first != n->words;
    if (temporary)
        scope_push();
    for (struct word *w = n->words; w != first; w = w->next) {
        const char *equal = strchr(w->text, '=');
        char *name = sh_slice(w->text, (size_t)(equal - w->text));
        char *value = expand_one(equal + 1, 0);
        if (temporary) {
            var_local(name, value);
            var_set(name, value, 1);
        } else {
            var_set(name, value, 0);
        }
        free(name);
        free(value);
    }

    struct fields args = expand_words(first);
    int result = 0;
    if (flow == FLOW_NORMAL && args.n) {
        const char *alias = alias_depth < 16 ? alias_get(args.v[0]) : NULL;
        if (alias) {
            size_t size = strlen(alias) + 2;
            for (int i = 1; i < args.n; i++)
                size += strlen(args.v[i]) * 4 + 3;
            char *line = sh_alloc(size);
            strcpy(line, alias);
            size_t used = strlen(line);
            for (int i = 1; i < args.n; i++) {
                line[used++] = ' ';
                line[used++] = '\'';
                for (const char *p = args.v[i]; *p; p++) {
                    if (*p == '\'') {
                        memcpy(line + used, "'\\''", 4);
                        used += 4;
                    } else {
                        line[used++] = *p;
                    }
                }
                line[used++] = '\'';
            }
            alias_depth++;
            int old_child = in_child;
            in_child = 0;
            result = run_line(line);
            in_child = old_child;
            alias_depth--;
            free(line);
        } else {
            const char *old_text = command_text;
            command_text = n->text;
            result = command_run(args.n, args.v, 1);
            command_text = old_text;
        }
    } else if (flow != FLOW_NORMAL) {
        result = last_status;
    } else if (substitutions != substitutions_before) {
        /* POSIX 2.9.1: without a command name the status is the one of
         * the last command substitution. */
        result = last_status;
    }
    fields_free(&args);
    if (temporary)
        scope_pop();
    return result;
}

static int pipeline(struct node *first, int background, int one)
{
    pid_t pids[MAX_CMDS], pgid = 0;
    int count = 0, previous = -1;
    fflush(NULL);
    for (struct node *n = first; n; n = one ? NULL : n->next) {
        if (count == MAX_CMDS) {
            fprintf(stderr, "sh: too many pipeline commands\n");
            goto fail;
        }
        int fds[2] = {-1, -1};
        if (!one && n->next && pipe(fds) < 0)
            goto fail;
        pid_t pid = fork();
        if (pid < 0) {
            if (fds[0] >= 0)
                close(fds[0]);
            if (fds[1] >= 0)
                close(fds[1]);
            goto fail;
        }
        if (!pid) {
            setpgid(0, pgid);
            child_signals();
            if (interactive && !background)
                tcsetpgrp(0, pgid ? pgid : getpid());
            if (previous >= 0) {
                dup2(previous, 0);
                close(previous);
            } else if (background && !interactive) {
                int null = open("/dev/null", O_RDONLY);
                if (null >= 0) {
                    dup2(null, 0);
                    close(null);
                }
            }
            if (fds[1] >= 0) {
                dup2(fds[1], 1);
                close(fds[0]);
                close(fds[1]);
            }
            interactive = 0;
            in_child = n->kind == N_SIMPLE;
            flow = FLOW_NORMAL;
            n->bg = 0;
            exit(execute_one(n));
        }
        if (!pgid)
            pgid = pid;
        pids[count++] = pid;
        setpgid(pid, pgid);
        if (interactive && !background)
            tcsetpgrp(0, pgid);
        if (previous >= 0)
            close(previous);
        if (fds[1] >= 0)
            close(fds[1]);
        previous = fds[0];
    }
    if (background) {
        last_background = pgid;
        job_add(pgid, pids, NULL, count, first->text ? first->text : "pipeline", JOB_RUNNING, interactive);
        return 0;
    }
    struct job job = {.used = 1, .state = JOB_RUNNING, .pgid = pgid, .npids = count};
    memcpy(job.pids, pids, (size_t)count * sizeof *pids);
    snprintf(job.text, sizeof job.text, "%s", first->text ? first->text : "pipeline");
    int result = wait_foreground(&job);
    if (interactive)
        tcsetpgrp(0, getpgrp());
    if (job.used && job.state == JOB_STOPPED)
        job_add(pgid, job.pids, job.statuses, count, job.text, JOB_STOPPED, 1);
    return result;

fail:
    perror("sh: pipeline");
    if (previous >= 0)
        close(previous);
    for (int i = 0; i < count; i++)
        kill(pids[i], SIGTERM);
    for (int i = 0; i < count; i++)
        wait_child(pids[i]);
    if (interactive)
        tcsetpgrp(0, getpgrp());
    return 1;
}

static int loop_control(void)
{
    if (flow != FLOW_BREAK && flow != FLOW_CONTINUE)
        return flow != FLOW_NORMAL;
    int stop = flow == FLOW_BREAK || flow_count > 1;
    if (--flow_count == 0)
        flow = FLOW_NORMAL;
    return stop;
}

static int compound(struct node *n)
{
    int result = 0;
    switch (n->kind) {
    case N_SIMPLE:
        return simple(n);
    case N_PIPELINE:
        if (n->op)
            errexit_off++;
        result = pipeline(n->a, n->bg, 0);
        if (n->op)
            errexit_off--;
        return n->op ? !result : result;
    case N_LIST:
        errexit_off++;
        result = exec_node(n->a);
        errexit_off--;
        if (flow == FLOW_NORMAL && ((n->op == T_AND && !result) || (n->op == T_OR && result)))
            result = exec_node(n->b);
        return result;
    case N_IF:
        errexit_off++;
        result = exec_node(n->a);
        errexit_off--;
        if (flow != FLOW_NORMAL)
            return result;
        return exec_node(result == 0 ? n->b : n->c);
    case N_FOR: {
        struct fields items = {0};
        if (n->op) {
            items = expand_words(n->words);
        } else {
            struct word all = {.text = "\"$@\""};
            items = expand_words(&all);
        }
        loop_depth++;
        for (int i = 0; i < items.n; i++) {
            var_set(n->text, items.v[i], 0);
            result = exec_node(n->a);
            if (loop_control())
                break;
        }
        loop_depth--;
        fields_free(&items);
        return result;
    }
    case N_WHILE:
    case N_UNTIL:
        loop_depth++;
        for (;;) {
            errexit_off++;
            int condition = exec_node(n->a);
            errexit_off--;
            if (flow != FLOW_NORMAL || ((condition == 0) != (n->kind == N_WHILE)))
                break;
            result = exec_node(n->b);
            if (loop_control())
                break;
        }
        loop_depth--;
        return result;
    case N_CASE: {
        char *value = expand_one(n->words->text, 0);
        int matched = 0;
        for (struct node *arm = n->a; arm && !matched; arm = arm->next) {
            for (struct word *w = arm->words; w; w = w->next) {
                char *pattern = expand_one(w->text, 1);
                matched = !fnmatch(pattern, value, 0);
                free(pattern);
                if (matched) {
                    result = exec_node(arm->a);
                    break;
                }
            }
        }
        free(value);
        return result;
    }
    case N_FUNCDEF:
        function_set(n->words->text, n->a);
        return 0;
    case N_SUBSHELL: {
        fflush(NULL);
        pid_t pid = fork();
        if (!pid) {
            interactive = 0;
            in_child = 0;
            exit(exec_node(n->a));
        }
        return pid < 0 ? 1 : wait_child(pid);
    }
    case N_GROUP:
        return exec_node(n->a);
    }
    return 2;
}

static int execute_one(struct node *n)
{
    if (n->bg && n->kind != N_PIPELINE)
        return pipeline(n, 1, 1);
    struct saved_fd *saved = NULL;
    int result;
    if (redirect_apply(n->redirs, &saved) < 0)
        result = 1;
    else
        result = compound(n);
    if (keep_redirects) {
        keep_redirects = 0;
        redirect_discard(saved);
    } else {
        redirect_restore(saved);
    }
    return result;
}

int opt_errexit, errexit_off, opt_noglob, opt_pipefail, keep_redirects;

int exec_node(struct node *n)
{
    int result = 0;
    for (; n && flow == FLOW_NORMAL; n = n->next) {
        result = execute_one(n);
        last_status = result;
        traps_run_pending();
        if (opt_errexit && result != 0 && errexit_off == 0 && !n->bg) {
            fflush(NULL);
            exit(result);
        }
    }
    return result;
}

int run_line(const char *line)
{
    int status;
    struct node *n = parse(line, &status);
    if (status) {
        fprintf(stderr, "sh: %s\n", status == 1 ? "unexpected end of input" : "syntax error");
        return last_status = 2;
    }
    int result = exec_node(n);
    node_free(n);
    return result;
}

char *capture(const char *cmd)
{
    int fds[2];
    if (pipe(fds) < 0)
        return strdup("");
    fflush(NULL);
    pid_t pid = fork();
    if (!pid) {
        dup2(fds[1], 1);
        close(fds[0]);
        close(fds[1]);
        interactive = 0;
        in_child = 0;
        flow = FLOW_NORMAL;
        exit(run_line(cmd));
    }
    close(fds[1]);
    char *out = sh_alloc(1);
    size_t length = 0;
    if (pid > 0) {
        char buf[1024];
        for (;;) {
            ssize_t n = read(fds[0], buf, sizeof buf);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                break;
            char *next = realloc(out, length + (size_t)n + 1);
            if (!next)
                exit(2);
            out = next;
            memcpy(out + length, buf, (size_t)n);
            length += (size_t)n;
        }
    }
    close(fds[0]);
    if (pid > 0)
        last_status = wait_child(pid);
    substitutions++;
    while (length && out[length - 1] == '\n')
        length--;
    out[length] = 0;
    return out;
}
