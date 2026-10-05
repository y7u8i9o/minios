#include "sh.h"
#include <sys/stat.h>

static const char *const names[] = {"cd",     "exit",     "pwd",     "export",  "unset", "set",    "jobs",
                                    "fg",     "bg",       "wait",    "true",    "false", "ulimit", "help",
                                    "test",   "[",        "echo",    "read",    "shift", "local",  "return",
                                    "source", ".",        "alias",   "unalias", "eval",  "type",   "command",
                                    "break",  "continue", "history", ":",       "umask", "exec",  "trap",   NULL};

int is_builtin(const char *name)
{
    for (int i = 0; names[i]; i++)
        if (!strcmp(names[i], name))
            return 1;
    return 0;
}

/* umask [-S] [mode]: print the file creation mask, in octal or with -S
 * as the permissions it leaves, or set it from an octal mode. */
static int builtin_umask(int argc, char **argv)
{
    int symbolic = argc > 1 && !strcmp(argv[1], "-S");
    int i = symbolic ? 2 : 1;
    if (i < argc) {
        char *end;
        unsigned long v = strtoul(argv[i], &end, 8);
        if (!argv[i][0] || *end || v > 0777) {
            fprintf(stderr, "umask: %s: octal mode expected\n", argv[i]);
            return 1;
        }
        umask((mode_t)v);
        return 0;
    }
    mode_t mask = umask(0);
    umask(mask);
    if (!symbolic) {
        printf("%04o\n", (unsigned)mask);
        return 0;
    }
    const char *who = "ugo";
    for (int k = 0; k < 3; k++) {
        unsigned allowed = ~mask >> (6 - 3 * k) & 7;
        printf("%s%c=%s%s%s", k ? "," : "", who[k], allowed & 4 ? "r" : "", allowed & 2 ? "w" : "",
               allowed & 1 ? "x" : "");
    }
    printf("\n");
    return 0;
}

static int echo(int argc, char **argv)
{
    int newline = 1, escapes = 0, i = 1;
    while (i < argc && argv[i][0] == '-' && argv[i][1] && strspn(argv[i] + 1, "neE") == strlen(argv[i] + 1)) {
        for (char *p = argv[i] + 1; *p; p++) {
            if (*p == 'n')
                newline = 0;
            if (*p == 'e')
                escapes = 1;
            if (*p == 'E')
                escapes = 0;
        }
        i++;
    }
    for (int first = i; i < argc; i++) {
        if (i != first)
            putchar(' ');
        for (const char *p = argv[i]; *p; p++) {
            if (!escapes || *p != '\\' || !p[1]) {
                putchar(*p);
                continue;
            }
            p++;
            if (*p == 'c')
                return 0;
            const char *key = strchr("abefnrtv\\", *p);
            if (key) {
                putchar("\a\b\033\f\n\r\t\v\\"[key - "abefnrtv\\"]);
            } else if (*p == '0') {
                int value = 0;
                for (int j = 0; j < 3 && p[1] >= '0' && p[1] <= '7'; j++)
                    value = value * 8 + (*++p - '0');
                putchar(value);
            } else {
                putchar('\\');
                putchar(*p);
            }
        }
    }
    if (newline)
        putchar('\n');
    return 0;
}

static int read_variables(int argc, char **argv)
{
    int first = 1, raw = 0;
    if (first < argc && !strcmp(argv[first], "-r")) {
        raw = 1;
        first++;
    }
    for (int i = first; i < argc; i++) {
        if (!valid_name(argv[i])) {
            fprintf(stderr, "read: %s: invalid name\n", argv[i]);
            return 2;
        }
    }
    size_t length = 0, capacity = 128;
    char *line = sh_alloc(capacity);
    int eof = 0;
    for (;;) {
        char c;
        ssize_t n = read(0, &c, 1);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            eof = 1;
            break;
        }
        if (c == '\n')
            break;
        if (!raw && c == '\\') {
            if (read(0, &c, 1) != 1) {
                eof = 1;
                break;
            }
            if (c == '\n')
                continue;
        }
        if (length + 1 >= capacity) {
            capacity *= 2;
            char *next = realloc(line, capacity);
            if (!next) {
                free(line);
                return 1;
            }
            line = next;
        }
        line[length++] = c;
    }
    line[length] = 0;
    const char *ifs = var_get("IFS");
    if (!ifs)
        ifs = " \t\n";
    if (first == argc) {
        var_set("REPLY", line, 0);
    } else {
        char *p = line;
        for (int i = first; i < argc; i++) {
            while (*p && strchr(ifs, *p) && strchr(" \t\n", *p))
                p++;
            char *start = p;
            if (i + 1 == argc) {
                p += strlen(p);
                while (p > start && strchr(ifs, p[-1]) && strchr(" \t\n", p[-1]))
                    p--;
                *p = 0;
            } else {
                while (*p && !strchr(ifs, *p))
                    p++;
                if (*p)
                    *p++ = 0;
            }
            var_set(argv[i], start, 0);
        }
    }
    free(line);
    return eof;
}

static int describe(const char *name, int path_only)
{
    const char *alias = alias_get(name);
    if (!path_only && alias) {
        printf("%s is aliased to '%s'\n", name, alias);
        return 0;
    }
    if (is_builtin(name) || function_get(name)) {
        if (path_only)
            puts(name);
        else
            printf("%s is a shell %s\n", name, is_builtin(name) ? "builtin" : "function");
        return 0;
    }
    const char *path = var_get("PATH");
    if (!path)
        path = "/bin";
    for (;;) {
        const char *end = strchr(path, ':');
        size_t n = end ? (size_t)(end - path) : strlen(path);
        char *candidate = sh_alloc(n + strlen(name) + 3);
        if (strchr(name, '/'))
            strcpy(candidate, name);
        else
            snprintf(candidate, n + strlen(name) + 3, "%.*s%s%s", (int)n, path, n ? "/" : "", name);
        struct stat st;
        int found = !stat(candidate, &st) && S_ISREG(st.st_mode) && (st.st_mode & 0111);
        if (found)
            printf(path_only ? "%s\n" : "%s is %s\n", path_only ? candidate : name, candidate);
        free(candidate);
        if (found)
            return 0;
        if (!end || strchr(name, '/'))
            break;
        path = end + 1;
    }
    if (!path_only)
        fprintf(stderr, "type: %s: not found\n", name);
    return 1;
}

/* The options of set: the letter, the name for -o and the flag. */
static const struct shell_option {
    char letter;
    const char *name;
    int *flag;
} shell_options[] = {
    { 'e', "errexit", &opt_errexit },
    { 'f', "noglob", &opt_noglob },
    { 0, "pipefail", &opt_pipefail },
};
#define SHELL_OPTIONS (int)(sizeof shell_options / sizeof shell_options[0])

/* shell_option returns the flag of the option with the letter, or with
 * the name when the letter is 0.  The result is NULL for an unknown
 * option. */
static int *shell_option(char letter, const char *name)
{
    for (int i = 0; i < SHELL_OPTIONS; i++)
        if (letter ? shell_options[i].letter == letter : !strcmp(shell_options[i].name, name))
            return shell_options[i].flag;
    return NULL;
}

/* set -o prints one line with the name and the state of each option.  set
 * +o prints the set commands that restore the current states. */
static void shell_options_print(int human)
{
    for (int i = 0; i < SHELL_OPTIONS; i++) {
        int on = *shell_options[i].flag;
        if (human)
            printf("%-12s%s\n", shell_options[i].name, on ? "on" : "off");
        else
            printf("set %co %s\n", on ? '-' : '+', shell_options[i].name);
    }
}


int builtin(int argc, char **argv)
{
    const char *name = argv[0];
    if (!strcmp(name, "exit") || !strcmp(name, "return")) {
        if (*name == 'r' && !function_depth) {
            fprintf(stderr, "return: not in a function or sourced file\n");
            return 2;
        }
        flow = *name == 'r' ? FLOW_RETURN : FLOW_EXIT;
        return argc > 1 ? atoi(argv[1]) & 255 : last_status;
    }
    if (!strcmp(name, "trap"))
        return builtin_trap(argc, argv);
    /* exec without a command makes the redirections of the command
     * permanent. With a command it replaces the shell, which exits when the
     * command cannot be executed. */
    if (!strcmp(name, "exec")) {
        if (argc == 1) {
            keep_redirects = 1;
            return 0;
        }
        fflush(NULL);
        child_signals();
        execvp(argv[1], argv + 1);
        fprintf(stderr, "exec: %s: %s\n", argv[1], strerror(errno));
        exit(errno == ENOENT ? 127 : 126);
    }
    if (!strcmp(name, "cd")) {
        const char *dir = argc > 1 ? argv[1] : var_get("HOME");
        if (!dir)
            dir = "/";
        if (!strcmp(dir, "-")) {
            dir = var_get("OLDPWD");
            if (!dir)
                return 1;
        }
        char old[1024], current[1024];
        int have_old = getcwd(old, sizeof old) != NULL;
        if (chdir(dir) < 0) {
            fprintf(stderr, "cd: %s: %s\n", dir, strerror(errno));
            return 1;
        }
        if (have_old)
            var_set("OLDPWD", old, 1);
        if (getcwd(current, sizeof current))
            var_set("PWD", current, 1);
        return 0;
    }
    if (!strcmp(name, "pwd")) {
        char path[1024];
        if (!getcwd(path, sizeof path))
            return 1;
        puts(path);
        return 0;
    }
    if (!strcmp(name, "export") || !strcmp(name, "local")) {
        int local = *name == 'l', result = 0;
        if (local && !function_depth) {
            fprintf(stderr, "local: not in a function\n");
            return 2;
        }
        if (argc == 1 && !local)
            vars_print();
        for (int i = 1; i < argc; i++) {
            char *equal = strchr(argv[i], '=');
            char *key = equal ? sh_slice(argv[i], (size_t)(equal - argv[i])) : strdup(argv[i]);
            const char *old = var_get(key);
            char *value = strdup(equal ? equal + 1 : old ? old : "");
            result |= local ? var_local(key, value) : var_set(key, value, 1);
            free(key);
            free(value);
        }
        return result;
    }
    if (!strcmp(name, "unset")) {
        for (int i = 1; i < argc; i++)
            var_unset(argv[i]);
        return 0;
    }
    if (!strcmp(name, "set")) {
        /* Options first: -e, -f and -o NAME, each turned off with + instead
         * of -.  Any other letter is refused.  -o or +o without a NAME
         * prints the options.  Then -- or the first non option word starts
         * the new positional parameters. */
        int i = 1;
        int replace = 0;
        for (; i < argc; i++) {
            if (!strcmp(argv[i], "--")) {
                i++;
                replace = 1;
                break;
            }
            if ((argv[i][0] != '-' && argv[i][0] != '+') || !argv[i][1]) {
                replace = 1;
                break;
            }
            const char *arg = argv[i];
            int on = arg[0] == '-';
            for (const char *p = arg + 1; *p; p++) {
                int *flag = shell_option(*p, NULL);
                if (*p == 'o' && i + 1 < argc) {
                    flag = shell_option(0, argv[++i]);
                    if (!flag) {
                        fprintf(stderr, "set: unsupported option %s\n", argv[i]);
                        return 2;
                    }
                } else if (*p == 'o') {
                    shell_options_print(on);
                    continue;
                } else if (!flag) {
                    fprintf(stderr, "set: unsupported option %c%c\n", arg[0], *p);
                    return 2;
                }
                *flag = on;
            }
        }
        if (replace) {
            /* Positional parameters outlive the expanded command arguments. */
            char **args = sh_alloc((size_t)(argc - i + 1) * sizeof *args);
            args[0] = script_argc ? script_argv[0] : "sh";
            for (int k = i; k < argc; k++)
                args[k - i + 1] = argv[k];
            parameters_replace(argc - i + 1, args);
            free(args);
        } else if (argc == 1) {
            vars_print();
        }
        return 0;
    }
    if (!strcmp(name, "shift")) {
        int count = argc > 1 ? atoi(argv[1]) : 1;
        if (count < 0 || count >= script_argc)
            return 1;
        int argc_new = script_argc - count;
        char **args = sh_alloc((size_t)(argc_new + 1) * sizeof *args);
        args[0] = script_argv[0];
        for (int i = 1; i < argc_new; i++)
            args[i] = script_argv[i + count];
        parameters_replace(argc_new, args);
        free(args);
        return 0;
    }
    if (!strcmp(name, "break") || !strcmp(name, "continue")) {
        int count = argc > 1 ? atoi(argv[1]) : 1;
        if (count < 1 || !loop_depth) {
            fprintf(stderr, "%s: not in a loop or invalid count\n", name);
            return 2;
        }
        flow_count = count < loop_depth ? count : loop_depth;
        flow = *name == 'b' ? FLOW_BREAK : FLOW_CONTINUE;
        return 0;
    }
    if (!strcmp(name, "source") || !strcmp(name, ".")) {
        if (argc < 2)
            return 2;
        function_depth++;
        int result = run_file(argv[1]);
        function_depth--;
        if (flow == FLOW_RETURN)
            flow = FLOW_NORMAL;
        return result;
    }
    if (!strcmp(name, "eval")) {
        size_t length = 1;
        for (int i = 1; i < argc; i++)
            length += strlen(argv[i]) + 1;
        char *text = sh_alloc(length);
        for (int i = 1; i < argc; i++) {
            if (i > 1)
                strcat(text, " ");
            strcat(text, argv[i]);
        }
        int result = run_line(text);
        free(text);
        return result;
    }
    if (!strcmp(name, "alias")) {
        int result = 0;
        if (argc == 1)
            alias_print();
        for (int i = 1; i < argc; i++) {
            char *equal = strchr(argv[i], '=');
            if (equal) {
                *equal = 0;
                alias_set(argv[i], equal + 1);
            } else {
                const char *value = alias_get(argv[i]);
                if (value)
                    printf("alias %s='%s'\n", argv[i], value);
                else
                    result = 1;
            }
        }
        return result;
    }
    if (!strcmp(name, "unalias")) {
        for (int i = 1; i < argc; i++)
            alias_unset(argv[i]);
        return 0;
    }
    if (!strcmp(name, "type") || !strcmp(name, "command")) {
        int command = *name == 'c';
        int describe_only = argc > 1 && (!strcmp(argv[1], "-v") || !strcmp(argv[1], "-V"));
        if (command && !describe_only)
            return command_run(argc - 1, argv + 1, 0);
        int result = 0;
        for (int i = describe_only ? 2 : 1; i < argc; i++)
            result |= describe(argv[i], command && !strcmp(argv[1], "-v"));
        return result;
    }
    if (!strcmp(name, "read"))
        return read_variables(argc, argv);
    if (!strcmp(name, "echo"))
        return echo(argc, argv);
    if (!strcmp(name, "test") || !strcmp(name, "["))
        return builtin_test(argc, argv);
    if (!strcmp(name, "jobs"))
        return builtin_jobs();
    if (!strcmp(name, "bg"))
        return builtin_bg(argv[1]);
    if (!strcmp(name, "fg"))
        return builtin_fg(argv[1]);
    if (!strcmp(name, "wait")) {
        jobs_reap(1);
        return 0;
    }
    if (!strcmp(name, "true") || !strcmp(name, ":"))
        return 0;
    if (!strcmp(name, "false"))
        return 1;
    if (!strcmp(name, "ulimit"))
        return builtin_ulimit(argv);
    if (!strcmp(name, "umask"))
        return builtin_umask(argc, argv);
    if (!strcmp(name, "history")) {
        shell_history(argc, argv);
        return 0;
    }
    if (!strcmp(name, "help")) {
        printf("builtins: cd exit pwd export");
        for (int i = 4; names[i]; i++)
            printf(" %s", names[i]);
        printf("\nquoting: '...' \"...\"; expansion: $name ${name} $(command) $((expression))\n");
        printf("lists: ; && || &; pipelines: |; control: if for while until case functions\n");
        printf("redirections: [fd]< > >> <& >& << <<-\n");
        return 0;
    }
    return -1;
}
