#include "sh.h"
#ifndef SH_TEST
#include <edit.h>
static struct edit *editor;
static char *history_path;
#endif

int interactive, last_status, script_argc, flow_count, loop_depth, function_depth;
char **script_argv;
pid_t last_background;
enum exec_flow flow;

int run_reader(struct reader *reader)
{
    char *line = NULL, *source = NULL;
    size_t capacity = 0, length = 0;
    int status = 0;
    while (flow == FLOW_NORMAL) {
        ssize_t count;
#ifndef SH_TEST
        if (reader->interactive && editor) {
            if (!line) {
                capacity = WORD_MAX;
                line = sh_alloc(capacity);
            }
            jobs_reap(0);
            char *prompt = prompt_render(length != 0);
            count = edit_readline(editor, prompt, line, capacity - 1);
            free(prompt);
            if (count == -EINTR) {
                length = 0;
                last_status = 130;
                continue;
            }
            if (count >= 0) {
                edit_history_add(editor, line);
                line[count++] = '\n';
                line[count] = 0;
            }
        } else
#endif
        {
            if (reader->interactive) {
                jobs_reap(0);
                char cwd[1024];
                if (length)
                    printf("> ");
                else
                    printf("%s $ ", getcwd(cwd, sizeof cwd) ? cwd : "?");
                fflush(stdout);
            }
            count = getline(&line, &capacity, reader->file);
        }
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
    struct reader reader = {.file = file};
    int result = run_reader(&reader);
    fclose(file);
    return result;
}

void shell_history(int argc, char **argv)
{
#ifndef SH_TEST
    if (!editor)
        return;
    for (size_t i = 0; i < edit_history_count(editor); i++)
        printf("%5lu  %s\n", (unsigned long)i + 1, edit_history_get(editor, i));
#endif
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
        prompt_startup();
        editor = edit_open(0, 1);
        if (!editor) {
            fprintf(stderr, "sh: cannot open line editor\n");
            return 1;
        }
        const char *limit = var_get("HISTSIZE");
        int count = limit ? atoi(limit) : 500;
        edit_history_limit(editor, count >= 0 && count <= 100000 ? (size_t)count : 500);
        const char *file = var_get("HISTFILE"), *home = var_get("HOME");
        if (file)
            history_path = strdup(file);
        else if (home) {
            size_t size = strlen(home) + 14;
            history_path = sh_alloc(size);
            snprintf(history_path, size, "%s/.sh_history", home);
        }
        if (history_path && *history_path)
            edit_history_load(editor, history_path);
    }
    struct reader reader = {.file = stdin, .interactive = interactive};
    int result = run_reader(&reader);
    jobs_reap(0);
    if (editor) {
        if (history_path && *history_path)
            edit_history_save(editor, history_path);
        edit_close(editor);
        free(history_path);
    }
    return result;
}
#endif
