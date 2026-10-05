/* Scripts that begin with #! (R1 of docs/plan/release-0.5.0.md), run by
 * the case shebang. The program is also the interpreter of its scripts:
 * started with an argument that begins with --echo, it writes its arguments and its
 * effective user id to the file that SHEBANG_OUT names. The checks start
 * scripts through execve directly, without the fallback of execvp: an
 * argument on the #! line with blanks around and inside it, arguments
 * after the script, a line with a carriage return, a line without a
 * newline, a script whose interpreter is a script, a chain that is too
 * deep, a missing interpreter, an empty and an overlong #! line, a script
 * without execute permission and a set user id script. Exits 0 on
 * success. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define SELF "/bin/shebangtest"
#define DIR  "/tmp/shebang"
#define OUT  DIR "/out"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

/* The interpreter mode: one line per argument, then the effective uid. */
static int echo_main(int argc, char **argv)
{
    const char *out = getenv("SHEBANG_OUT");
    FILE *f = fopen(out ? out : OUT, "w");
    if (!f)
        return 3;
    for (int i = 0; i < argc; i++)
        fprintf(f, "%d:%s\n", i, argv[i]);
    fprintf(f, "euid:%d\n", (int)geteuid());
    fclose(f);
    return 0;
}

static void write_script(const char *path, const char *text, mode_t mode)
{
    unlink(path);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    CHECK(fd >= 0, "create %s: %s", path, strerror(errno));
    if (fd < 0)
        return;
    CHECK(write(fd, text, strlen(text)) == (ssize_t)strlen(text), "write %s", path);
    close(fd);
    CHECK(chmod(path, mode) == 0, "chmod %s: %s", path, strerror(errno));
}

static const char *read_out(void)
{
    static char buf[1024];
    buf[0] = '\0';
    int fd = open(OUT, O_RDONLY);
    if (fd < 0)
        return buf;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    buf[n > 0 ? n : 0] = '\0';
    close(fd);
    return buf;
}

/* Start path with argv in a child, optionally as uid, and return the
 * errno of a failed execve, or 0 when the child ran and exited with 0.
 * A child that ran and failed returns -1. */
static int run(const char *path, char *const argv[], int uid)
{
    unlink(OUT);
    int pipefd[2];
    if (pipe(pipefd) < 0)
        return -1;
    pid_t pid = fork();
    if (pid == 0) {
        close(pipefd[0]);
        fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);
        if (uid >= 0 && setuid((uid_t)uid) < 0)
            _exit(5);
        char *envp[] = { "SHEBANG_OUT=" OUT, "PATH=/bin", NULL };
        execve(path, argv, envp);
        int e = errno;
        write(pipefd[1], &e, sizeof e);
        _exit(4);
    }
    close(pipefd[1]);
    int e = 0;
    ssize_t n = read(pipefd[0], &e, sizeof e);
    close(pipefd[0]);
    int status;
    waitpid(pid, &status, 0);
    if (n == (ssize_t)sizeof e)
        return e;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

static void expect_output(const char *what, const char *path, char *const argv[], const char *want)
{
    int r = run(path, argv, -1);
    CHECK(r == 0, "%s: run returned %d (%s)", what, r, r > 0 ? strerror(r) : "child failed");
    const char *got = read_out();
    CHECK(strcmp(got, want) == 0, "%s: output\n%s\nexpected\n%s", what, got, want);
}

static void expect_errno(const char *what, const char *path, char *const argv[], int want)
{
    int r = run(path, argv, -1);
    CHECK(r == want, "%s: execve gave %d (%s), expected %d (%s)", what, r, r > 0 ? strerror(r) : "-",
          want, strerror(want));
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
        if (strncmp(argv[i], "--echo", 6) == 0)
            return echo_main(argc, argv);
    mkdir(DIR, 0777);
    chmod(DIR, 0777);

    /* The argument of the #! line is one word, with the blanks around it
     * removed and the blanks inside it retained. */
    write_script(DIR "/arg", "#!  " SELF "   --echo  two words \t\nbody\n", 0755);
    expect_output("argument", DIR "/arg", (char *[]){ "arg", "x", "y z", NULL },
                  "0:" SELF "\n1:--echo  two words\n2:" DIR "/arg\n3:x\n4:y z\neuid:0\n");

    /* Without the argument, the script path follows the interpreter. */
    write_script(DIR "/noarg", "#!" SELF "\n", 0755);
    expect_output("no argument", DIR "/noarg", (char *[]){ "noarg", "--echo", NULL },
                  "0:" SELF "\n1:" DIR "/noarg\n2:--echo\neuid:0\n");

    /* argv[0] of the caller is replaced; an empty vector still passes
     * the path. */
    expect_output("empty argv", DIR "/arg", (char *[]){ NULL },
                  "0:" SELF "\n1:--echo  two words\n2:" DIR "/arg\neuid:0\n");

    /* A carriage return at the end of the line is not part of it. */
    write_script(DIR "/crlf", "#!" SELF " --echo\r\n", 0755);
    expect_output("carriage return", DIR "/crlf", (char *[]){ "crlf", NULL },
                  "0:" SELF "\n1:--echo\n2:" DIR "/crlf\neuid:0\n");

    /* A file that ends without a newline is one line. */
    write_script(DIR "/nonl", "#!" SELF " --echo", 0755);
    expect_output("no newline", DIR "/nonl", (char *[]){ "nonl", NULL },
                  "0:" SELF "\n1:--echo\n2:" DIR "/nonl\neuid:0\n");

    /* An interpreter that is itself a script. */
    write_script(DIR "/inner", "#!" SELF " --echo\n", 0755);
    write_script(DIR "/outer", "#!" DIR "/inner -o\n", 0755);
    expect_output("nested", DIR "/outer", (char *[]){ "outer", "a", NULL },
                  "0:" SELF "\n1:--echo\n2:" DIR "/inner\n3:-o\n4:" DIR "/outer\n5:a\neuid:0\n");

    /* Four scripts in a chain reach the limit. */
    write_script(DIR "/l1", "#!" DIR "/inner\n", 0755);
    write_script(DIR "/l2", "#!" DIR "/l1\n", 0755);
    write_script(DIR "/l3", "#!" DIR "/l2\n", 0755);
    write_script(DIR "/l4", "#!" DIR "/l3\n", 0755);
    expect_output("depth 4", DIR "/l3", (char *[]){ "l3", NULL },
                  "0:" SELF "\n1:--echo\n2:" DIR "/inner\n3:" DIR "/l1\n4:" DIR "/l2\n5:" DIR "/l3\neuid:0\n");
    expect_errno("depth 5", DIR "/l4", (char *[]){ "l4", NULL }, ELOOP);

    /* Errors of the interpreter line and of the script itself. */
    write_script(DIR "/missing", "#!/bin/no-such-interpreter\n", 0755);
    expect_errno("missing interpreter", DIR "/missing", (char *[]){ "missing", NULL }, ENOENT);
    write_script(DIR "/empty", "#!   \nexit 0\n", 0755);
    expect_errno("empty line", DIR "/empty", (char *[]){ "empty", NULL }, ENOEXEC);
    char longline[400] = "#!" SELF " --echo ";
    while (strlen(longline) < 300)
        strcat(longline, "x");
    strcat(longline, "\n");
    write_script(DIR "/long", longline, 0755);
    expect_errno("long line", DIR "/long", (char *[]){ "long", NULL }, ENOEXEC);
    write_script(DIR "/noexec", "#!" SELF " --echo\n", 0644);
    expect_errno("no execute permission", DIR "/noexec", (char *[]){ "noexec", NULL }, EACCES);

    /* The set user id bit of a script has no effect: an unprivileged user
     * runs it with its own id. */
    write_script(DIR "/setuid", "#!" SELF " --echo\n", 04755);
    int r = run(DIR "/setuid", (char *[]){ "setuid", NULL }, 1000);
    CHECK(r == 0, "set user id script: run returned %d", r);
    const char *got = read_out();
    CHECK(strstr(got, "euid:1000\n") != NULL, "set user id script: output\n%s", got);

    printf("shebangtest: %d failures\n", failures);
    return failures ? 1 : 0;
}
