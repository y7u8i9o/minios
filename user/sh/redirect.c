#include "sh.h"

static int here_pipe(const struct redir *r, pid_t *writer)
{
    char *body = r->quoted ? strdup(r->body) : expand_here(r->body);
    int fds[2];
    if (pipe(fds) < 0) {
        free(body);
        return -1;
    }
    fflush(NULL);
    *writer = fork();
    if (*writer == 0) {
        signal(SIGPIPE, SIG_DFL);
        close(fds[0]);
        size_t length = strlen(body), offset = 0;
        while (offset < length) {
            ssize_t n = write(fds[1], body + offset, length - offset);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                _exit(1);
            offset += (size_t)n;
        }
        close(fds[1]);
        _exit(0);
    }
    free(body);
    close(fds[1]);
    if (*writer < 0) {
        close(fds[0]);
        return -1;
    }
    return fds[0];
}

int redirect_apply(struct redir *r, struct saved_fd **saved)
{
    int minimum = 10;
    for (struct redir *it = r; it; it = it->next)
        if (it->fd >= minimum)
            minimum = it->fd + 1;

    fflush(NULL);
    for (; r; r = r->next) {
        struct saved_fd *s = sh_alloc(sizeof *s);
        s->fd = r->fd;
        s->saved = fcntl(r->fd, F_DUPFD_CLOEXEC, minimum);
        if (s->saved < 0 && errno != EBADF) {
            free(s);
            return -1;
        }
        s->next = *saved;
        *saved = s;

        char *target = expand_one(r->target->text, 0);
        int fd = -1;
        int close_after = 1;
        switch (r->kind) {
        case R_IN:
            fd = open(target, O_RDONLY);
            break;
        case R_OUT:
        case R_APPEND:
            fd = open(target, O_WRONLY | O_CREAT | (r->kind == R_APPEND ? O_APPEND : O_TRUNC), 0644);
            break;
        case R_DUPIN:
        case R_DUPOUT:
            if (!strcmp(target, "-")) {
                close(r->fd);
                free(target);
                continue;
            }
            char *end;
            long number = strtol(target, &end, 10);
            if (!*target || *end || number < 0 || number > 255) {
                errno = EBADF;
            } else {
                fd = (int)number;
            }
            close_after = 0;
            break;
        case R_HEREDOC:
            fd = here_pipe(r, &s->writer);
            break;
        }
        int failed = fd < 0 || dup2(fd, r->fd) < 0;
        if (close_after && fd >= 0 && fd != r->fd)
            close(fd);
        if (failed)
            fprintf(stderr, "sh: %s: %s\n", target, strerror(errno));
        free(target);
        if (failed)
            return -1;
    }
    return 0;
}

void redirect_restore(struct saved_fd *saved)
{
    fflush(NULL);
    clearerr(stdin);
    clearerr(stdout);
    clearerr(stderr);
    while (saved) {
        struct saved_fd *next = saved->next;
        if (saved->saved >= 0) {
            dup2(saved->saved, saved->fd);
            close(saved->saved);
        } else {
            close(saved->fd);
        }
        if (saved->writer > 0) {
            int status;
            while (waitpid(saved->writer, &status, 0) < 0 && errno == EINTR)
                ;
        }
        free(saved);
        saved = next;
    }
}
