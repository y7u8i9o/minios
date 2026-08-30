/* M17 stage 5: pseudo terminals. A shell runs on the slave; the master
 * side feeds input, reads output, and checks control C, window size,
 * raw mode and hangup. Exits 0 on success. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/ipc.h>

static int failures;
static volatile int winch_seen;

static void on_winch(int sig)
{
    winch_seen = 1;
}
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

/* Read from the master until text appears or the timeout passes. */
static int expect(int master, const char *text, char *out, size_t size)
{
    size_t len = 0;
    for (int i = 0; i < 200; i++) {
        struct pollfd p = { master, POLLIN, 0 };
        if (poll(&p, 1, 50) > 0) {
            ssize_t n = read(master, out + len, size - len - 1);
            if (n > 0) {
                len += (size_t)n;
                out[len] = '\0';
            }
        }
        if (strstr(out, text))
            return 1;
    }
    return 0;
}

int main(void)
{
    printf("ptytest: pid %d\n", getpid());
    int master;
    char slave[32];
    CHECK(openpty(&master, slave, sizeof slave) == 0, "openpty: %s", strerror(errno));
    CHECK(strncmp(slave, "/dev/pts", 8) == 0, "slave path %s", slave);
    struct winsize ws = { 30, 100 };
    CHECK(ioctl(master, TIOCSWINSZ, &ws) == 0, "set window size");

    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        int s = open(slave, O_RDWR);
        if (s < 0)
            _exit(2);
        dup2(s, 0);
        dup2(s, 1);
        dup2(s, 2);
        close(s);
        close(master);
        tcsetpgrp(0, getpgrp());
        char *const args[] = { "sh", NULL };
        execvp("sh", args);
        _exit(127);
    }
    char buf[2048] = "";
    CHECK(expect(master, "$ ", buf, sizeof buf), "prompt: '%s'", buf);

    /* Echo of the input line comes back through the line discipline,
     * then the command output. */
    write(master, "echo pty works\n", 15);
    buf[0] = '\0';
    CHECK(expect(master, "pty works\n", buf, sizeof buf), "echo output: '%s'", buf);
    CHECK(strstr(buf, "echo pty works") != NULL, "input echoed: '%s'", buf);

    /* Control C interrupts a foreground program. */
    write(master, "cat\n", 4);
    sleep_ms(300);
    write(master, "\003", 1);
    buf[0] = '\0';
    CHECK(expect(master, "terminated by signal 2", buf, sizeof buf), "control C: '%s'", buf);

    /* The slave sees the window size set on the master. */
    write(master, "sh -c 'exit 0'\n", 15);
    int s = open(slave, O_RDONLY);
    struct winsize got = { 0, 0 };
    CHECK(s >= 0 && ioctl(s, TIOCGWINSZ, &got) == 0 && got.ws_row == 30 && got.ws_col == 100,
          "window size %ux%u", got.ws_col, got.ws_row);
    close(s);

    /* Background job output arrives while the shell waits for input. */
    write(master, "echo from > /ptytest.out; cat /ptytest.out; rm /ptytest.out\n", 60);
    buf[0] = '\0';
    CHECK(expect(master, "from\n", buf, sizeof buf), "file round trip: '%s'", buf);
    /* Wait for the prompt: the hangup below must not orphan a child of
     * the shell that is still running or not yet reaped, since nothing
     * reaps orphans while init is not running. */
    buf[0] = '\0';
    CHECK(expect(master, "$ ", buf, sizeof buf), "prompt after the round trip: '%s'", buf);

    /* Closing the master hangs the slave up: the shell exits. */
    close(master);
    int status = 0;
    pid_t r;
    for (int i = 0; i < 100; i++) {
        r = waitpid(pid, &status, WNOHANG);
        if (r == pid)
            break;
        sleep_ms(50);
    }
    CHECK(r == pid, "shell exited after hangup (status 0x%x)", status);
    CHECK(open(slave, O_RDWR) < 0 && errno == ENXIO, "slave without master refused");

    /* A second pair is independent and raw mode delivers bytes at once. */
    CHECK(openpty(&master, slave, sizeof slave) == 0, "second openpty");
    s = open(slave, O_RDWR);
    struct termios t;
    CHECK(tcgetattr(s, &t) == 0 && (t.c_lflag & ICANON), "default canonical");
    t.c_lflag &= ~(ICANON | ECHO);
    CHECK(tcsetattr(s, TCSANOW, &t) == 0, "raw mode");
    write(master, "ab", 2);
    char two[4] = "";
    CHECK(read(s, two, 2) == 2 && two[0] == 'a' && two[1] == 'b', "raw bytes '%s'", two);
    close(s);

    /* A window size change on the master sends SIGWINCH to the slave's
     * foreground process group. */
    pid_t wp = fork();
    if (wp == 0) {
        signal(SIGWINCH, on_winch);
        setpgid(0, 0);
        int fd = open(slave, O_RDWR);
        if (fd < 0)
            _exit(2);
        tcsetpgrp(fd, getpgrp());
        for (int i = 0; i < 100 && !winch_seen; i++)
            sleep_ms(20);
        _exit(winch_seen ? 0 : 3);
    }
    sleep_ms(200);
    struct winsize ws2 = { 40, 120 };
    CHECK(ioctl(master, TIOCSWINSZ, &ws2) == 0, "second window size");
    status = -1;
    waitpid(wp, &status, 0);
    CHECK(status == 0, "SIGWINCH delivered to the foreground group (status 0x%x)", status);
    close(master);
    printf("ptytest: %d failures\n", failures);
    return failures ? 1 : 0;
}
