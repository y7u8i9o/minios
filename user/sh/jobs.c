#include "sh.h"
static struct job jobs[MAX_JOBS];
static int job_alive(const struct job *j)
{
    for (int i = 0; i < j->npids; i++)
        if (j->pids[i] > 0)
            return 1;
    return 0;
}

int job_add(pid_t pgid, pid_t *pids, const int *statuses, int n, const char *text,
                   int state, int announce)
{
    for (int i = 0; i < MAX_JOBS; i++) {
        if (!jobs[i].used) {
            jobs[i].used = 1;
            jobs[i].state = state;
            jobs[i].pgid = pgid;
            jobs[i].npids = n;
            memcpy(jobs[i].pids, pids, (size_t)n * sizeof pids[0]);
            if (statuses)
                memcpy(jobs[i].statuses, statuses, (size_t)n * sizeof statuses[0]);
            else
                memset(jobs[i].statuses, 0, sizeof jobs[i].statuses);
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
        if (j->pids[i] == pid) {
            j->pids[i] = 0;
            j->statuses[i] = WIFSIGNALED(status) ? 128 + WTERMSIG(status) : WEXITSTATUS(status);
        }
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
void jobs_reap(int block)
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

int wait_foreground(struct job *j)
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
    /* With pipefail the rightmost failed command decides, as in bash.  A
     * command that a SIGPIPE ended counts as failed. */
    if (opt_pipefail && !job_alive(j)) {
        result = 0;
        for (int i = 0; i < j->npids; i++)
            if (j->statuses[i] != 0)
                result = j->statuses[i];
    }
    if (!job_alive(j))
        j->used = 0;
    return result;
}

int builtin_bg(const char *arg)
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

int builtin_fg(const char *arg)
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


int builtin_jobs(void)
{
    jobs_reap(0);
    for (int i = 0; i < MAX_JOBS; i++)
        if (jobs[i].used) printf("[%d] %s  %s\n", i + 1, jobs[i].state == JOB_STOPPED ? "Stopped" : "Running", jobs[i].text);
    return 0;
}

