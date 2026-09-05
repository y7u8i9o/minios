#include "sh.h"

int interactive, last_status, script_argc, flow_count, loop_depth, function_depth;
char **script_argv;
enum exec_flow flow;

int run_reader(struct reader *reader)
{
    char *line = NULL, *source = NULL;
    size_t capacity = 0, length = 0;
    int status = 0;
    while (flow == FLOW_NORMAL) {
        if (reader->interactive) {
            jobs_reap(0);
            char cwd[1024];
            if (length)
                printf("> ");
            else
                printf("%s $ ", getcwd(cwd, sizeof cwd) ? cwd : "?");
            fflush(stdout);
        }
        ssize_t count = getline(&line, &capacity, reader->file);
        if (count < 0) {
            if (length) {
                fprintf(stderr, "sh: unexpected end of input\n");
                last_status = 2;
            }
            break;
        }
        char *next = realloc(source, length + (size_t)count + 1);
        if (!next)
            exit(2);
        source = next;
        memcpy(source + length, line, (size_t)count + 1);
        length += (size_t)count;
        struct node *n = parse(source, &status);
        if (status == 1)
            continue;
        if (status == 2) {
            fprintf(stderr, "sh: syntax error\n");
            last_status = 2;
        } else if (n) {
            last_status = exec_node(n);
        }
        node_free(n);
        length = 0;
        if (status == 2 && !reader->interactive)
            break;
    }
    free(line);
    free(source);
    return last_status;
}

int run_file(const char *path)
{
    FILE *file = fopen(path, "r");
    if (!file) {
        fprintf(stderr, "sh: %s: %s\n", path, strerror(errno));
        return 1;
    }
    fcntl(fileno(file), F_SETFD, FD_CLOEXEC);
    struct reader reader = { .file = file };
    int result = run_reader(&reader);
    fclose(file);
    return result;
}

/* Replaced by the libedit history integration in the interactive stage. */
void shell_history(int argc, char **argv)
{
    (void)argc;
    (void)argv;
}

#ifndef SH_TEST
int main(int argc, char **argv)
{
    vars_init();
    script_argc = 1;
    script_argv = argv;
    if (argc > 1 && !strcmp(argv[1], "-c")) {
        if (argc < 3) {
            fprintf(stderr, "sh: -c requires a command\n");
            return 2;
        }
        if (argc > 3) {
            script_argc = argc - 3;
            script_argv = argv + 3;
        }
        return run_line(argv[2]);
    }
    if (argc > 1) {
        script_argc = argc - 1;
        script_argv = argv + 1;
        return run_file(argv[1]);
    }
    interactive = isatty(0);
    if (interactive) {
        signal(SIGINT, SIG_IGN);
        signal(SIGPIPE, SIG_IGN);
        signal(SIGTSTP, SIG_IGN);
        signal(SIGTTIN, SIG_IGN);
        signal(SIGTTOU, SIG_IGN);
        setpgid(0, 0);
        tcsetpgrp(0, getpgrp());
    }
    struct reader reader = { .file = stdin, .interactive = interactive };
    int result = run_reader(&reader);
    jobs_reap(0);
    return result;
}
#endif
