/* U2 test: permission enforcement. Started as root by the perm_user case,
 * it builds a tree under /tmp/perm and checks in children running as uid
 * 1000 that the kernel refuses what the permission bits, the sticky bit
 * and the privilege rules forbid and allows the rest, and that root keeps
 * its access. Copies of the program serve as the setuid helper:
 * "--secure" reports AT_SECURE and the ids, "--access PATH" compares
 * access and faccessat with AT_EACCESS. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <netinet/in.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)
/* The call failed with the given errno. */
#define FAILS(call, e) ((call) < 0 && errno == (e))

#define AT_SECURE_TYPE 23

static char **saved_envp;

/* Wait for a signal. */
static void wait_forever(void)
{
    for (;;)
        sleep(100);
}

/* The auxiliary vector follows the environment on the initial stack. */
static long aux_value(long type)
{
    char **p = saved_envp;
    while (*p)
        p++;
    for (long *aux = (long *)(p + 1); aux[0] != 0; aux += 2)
        if (aux[0] == type)
            return aux[1];
    return -1;
}

static void write_file(const char *path, const char *text, mode_t mode)
{
    unlink(path);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        write(fd, text, strlen(text));
        close(fd);
    }
    chmod(path, mode);
}

static int copy_file(const char *from, const char *to, mode_t mode)
{
    int in = open(from, O_RDONLY);
    if (in < 0)
        return -1;
    unlink(to);
    int out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0700);
    if (out < 0) {
        close(in);
        return -1;
    }
    char buf[4096];
    ssize_t n;
    while ((n = read(in, buf, sizeof buf)) > 0)
        write(out, buf, (size_t)n);
    close(in);
    close(out);
    return chmod(to, mode);
}

/* Become uid and gid 1000 the way login does. */
static void drop(void)
{
    gid_t none[1];
    if (setgroups(0, none) < 0 || setgid(1000) < 0 || setuid(1000) < 0) {
        printf("permtest: cannot drop to uid 1000\n");
        _exit(100);
    }
}

/* Run fn as uid 1000 in a child; its return value is the number of
 * failed checks, added to ours. */
static void as_user(const char *what, void (*fn)(void))
{
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        drop();
        failures = 0;
        fn();
        fflush(stdout);
        _exit(failures > 99 ? 99 : failures);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status)) {
        failures++;
        printf("FAIL: %s: child status %x\n", what, status);
    } else {
        failures += WEXITSTATUS(status);
    }
}

/* Run argv with stdout into buf and return the exit status, or -1. */
static int run_capture(char *const argv[], char *buf, size_t size)
{
    int fds[2];
    if (pipe(fds) < 0)
        return -1;
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        dup2(fds[1], 1);
        close(fds[0]);
        close(fds[1]);
        execv(argv[0], argv);
        _exit(127);
    }
    close(fds[1]);
    size_t used = 0;
    ssize_t n;
    while (used + 1 < size && (n = read(fds[0], buf + used, size - 1 - used)) > 0)
        used += (size_t)n;
    buf[used] = '\0';
    close(fds[0]);
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void setup(void)
{
    mkdir("/tmp/perm", 0755);
    chmod("/tmp/perm", 0755);
    write_file("/tmp/perm/secret", "secret\n", 0600);
    write_file("/tmp/perm/public", "public\n", 0644);
    write_file("/tmp/perm/shared", "shared\n", 0666);
    mkdir("/tmp/perm/private", 0700);
    write_file("/tmp/perm/private/f", "inside\n", 0644);
    mkdir("/tmp/perm/open", 0777);
    chmod("/tmp/perm/open", 0777);
    write_file("/tmp/perm/open/rootfile", "root\n", 0644);
    mkdir("/tmp/perm/open2", 0777);
    chmod("/tmp/perm/open2", 0777);
    mkdir("/tmp/perm/sticky", 0777);
    chmod("/tmp/perm/sticky", 01777);
    write_file("/tmp/perm/sticky/rootfile", "root\n", 0666);
    copy_file("/bin/id", "/tmp/perm/id-noexec", 0644);
    copy_file("/bin/id", "/tmp/perm/id-execonly", 0711);
    copy_file("/bin/permtest", "/tmp/perm/suid", 04755);
    chown("/tmp/perm/suid", 0, 0);
    chmod("/tmp/perm/suid", 04755);
}

static void user_files(void)
{
    CHECK(FAILS(open("/tmp/perm/secret", O_RDONLY), EACCES), "read of a 0600 file of root");
    int fd = open("/tmp/perm/public", O_RDONLY);
    CHECK(fd >= 0, "read of a 0644 file: %s", strerror(errno));
    if (fd >= 0)
        close(fd);
    CHECK(FAILS(open("/tmp/perm/public", O_WRONLY), EACCES), "write of a 0644 file of root");
    CHECK(FAILS(open("/tmp/perm/public", O_RDONLY | O_TRUNC), EACCES), "truncation of a 0644 file");
    fd = open("/tmp/perm/shared", O_RDWR);
    CHECK(fd >= 0, "0666 file not writable: %s", strerror(errno));
    if (fd >= 0)
        close(fd);
    struct stat st;
    CHECK(FAILS(stat("/tmp/perm/private/f", &st), EACCES), "search of a 0700 directory");
    CHECK(FAILS(open("/tmp/perm/private", O_RDONLY), EACCES), "listing of a 0700 directory");
    CHECK(FAILS(chdir("/tmp/perm/private"), EACCES), "chdir into a 0700 directory");
    CHECK(FAILS(open("/tmp/perm/new", O_WRONLY | O_CREAT, 0644), EACCES), "create in a 0755 directory");
    CHECK(FAILS(mkdir("/tmp/perm/newdir", 0755), EACCES), "mkdir in a 0755 directory");
    CHECK(FAILS(unlink("/tmp/perm/public"), EACCES), "unlink in a 0755 directory");
    CHECK(FAILS(rename("/tmp/perm/public", "/tmp/perm/open/public"), EACCES), "rename out of a 0755 directory");
    CHECK(FAILS(link("/tmp/perm/public", "/tmp/perm/link"), EACCES), "link in a 0755 directory");
    CHECK(FAILS(symlink("public", "/tmp/perm/sym"), EACCES), "symlink in a 0755 directory");

    /* A file created with mode 0444 is still open for writing. */
    fd = open("/tmp/perm/open/mine", O_RDWR | O_CREAT | O_EXCL, 0444);
    CHECK(fd >= 0, "create in a 0777 directory: %s", strerror(errno));
    if (fd >= 0) {
        CHECK(write(fd, "x", 1) == 1, "write to a file the call created read only");
        close(fd);
    }
    CHECK(unlink("/tmp/perm/open/rootfile") == 0, "unlink of root's file in a 0777 directory: %s",
          strerror(errno));
    CHECK(mkdir("/tmp/perm/open/cd", 0777) == 0 && chmod("/tmp/perm/open/cd", 0555) == 0, "mkdir cd");
    CHECK(rename("/tmp/perm/open/cd", "/tmp/perm/open/cd2") == 0, "rename of a 0555 directory in place");
    CHECK(FAILS(rename("/tmp/perm/open/cd2", "/tmp/perm/open2/cd"), EACCES),
          "a 0555 directory moved to another parent");
    chmod("/tmp/perm/open/cd2", 0755);
    CHECK(rename("/tmp/perm/open/cd2", "/tmp/perm/open2/cd") == 0, "a 0755 directory moved: %s", strerror(errno));

    /* The sticky bit. */
    CHECK(FAILS(unlink("/tmp/perm/sticky/rootfile"), EPERM), "unlink of root's file in a sticky directory");
    write_file("/tmp/perm/sticky/mine", "mine\n", 0644);
    CHECK(FAILS(rename("/tmp/perm/sticky/mine", "/tmp/perm/sticky/rootfile"), EPERM),
          "rename over root's file in a sticky directory");
    CHECK(rename("/tmp/perm/sticky/mine", "/tmp/perm/sticky/mine2") == 0, "rename of an own file: %s",
          strerror(errno));
    CHECK(unlink("/tmp/perm/sticky/mine2") == 0, "unlink of an own file in a sticky directory");

    /* Times. */
    CHECK(utimensat(AT_FDCWD, "/tmp/perm/shared", NULL, 0) == 0, "touch of a 0666 file: %s", strerror(errno));
    struct timespec ts[2] = { { 0, UTIME_OMIT }, { 1000000, 0 } };
    CHECK(FAILS(utimensat(AT_FDCWD, "/tmp/perm/shared", ts, 0), EPERM), "explicit time on another's file");
    CHECK(utimensat(AT_FDCWD, "/tmp/perm/open/mine", ts, 0) == 0, "explicit time on an own file");
    CHECK(FAILS(utimensat(AT_FDCWD, "/tmp/perm/public", NULL, 0), EACCES), "touch of a 0644 file of root");

    /* access with the real ids. */
    CHECK(access("/tmp/perm/public", R_OK) == 0, "access R_OK");
    CHECK(FAILS(access("/tmp/perm/public", W_OK), EACCES), "access W_OK");
    CHECK(FAILS(access("/tmp/perm/private/f", F_OK), EACCES), "access through a 0700 directory");
    CHECK(access("/tmp/perm/id-execonly", X_OK) == 0, "access X_OK");
}

static void user_exec(void)
{
    char out[512];
    char *noexec[] = { "/tmp/perm/id-noexec", NULL };
    CHECK(FAILS(execv(noexec[0], noexec), EACCES), "exec of a 0644 file");
    char *execonly[] = { "/tmp/perm/id-execonly", "-u", NULL };
    CHECK(run_capture(execonly, out, sizeof out) == 0 && strcmp(out, "1000\n") == 0,
          "exec of a 0711 file: %s", out);
    char *suid[] = { "/tmp/perm/suid", "--secure", NULL };
    CHECK(run_capture(suid, out, sizeof out) == 0 && strcmp(out, "secure 1 ruid 1000 euid 0 suid 0\n") == 0,
          "setuid program: %s", out);
    char *acc[] = { "/tmp/perm/suid", "--access", "/tmp/perm/secret", NULL };
    CHECK(run_capture(acc, out, sizeof out) == 0 && strcmp(out, "access EACCES eaccess 0 open 0\n") == 0,
          "access in a setuid program: %s", out);
    char *self[] = { "/bin/permtest", "--secure", NULL };
    CHECK(run_capture(self, out, sizeof out) == 0 && strcmp(out, "secure 0 ruid 1000 euid 1000 suid 1000\n") == 0,
          "plain program: %s", out);
}

static void user_privileged(void)
{
    CHECK(FAILS(mount("vdb", "/tmp/perm/open", "mfs"), EPERM), "mount");
    CHECK(FAILS(umount("/dev"), EPERM), "umount");
    CHECK(FAILS(reboot(RB_POWER_OFF), EPERM), "reboot");
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    CHECK(FAILS(clock_settime(CLOCK_REALTIME, &now), EPERM), "clock_settime");
    struct rlimit rl;
    getrlimit(RLIMIT_CORE, &rl);
    struct rlimit lower = { 0, rl.rlim_max == RLIM_INFINITY ? 1000000 : rl.rlim_max - 1 };
    CHECK(setrlimit(RLIMIT_CORE, &lower) == 0, "lowering a hard limit: %s", strerror(errno));
    CHECK(FAILS(setrlimit(RLIMIT_CORE, &rl), EPERM), "raising a hard limit");
    CHECK(FAILS(prlimit(1, RLIMIT_CORE, NULL, &rl), EPERM), "limits of a process of root");
    int net = open("/dev/net", O_RDONLY);
    struct net_config cfg;
    memset(&cfg, 0, sizeof cfg);
    CHECK(net >= 0 && FAILS(ioctl(net, NETIOC_CONFIGURE, &cfg), EPERM), "network configuration");
    if (net >= 0)
        close(net);
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(80);
    CHECK(s >= 0 && FAILS(bind(s, (struct sockaddr *)&a, sizeof a), EACCES), "bind to port 80");
    a.sin_port = htons(5080);
    CHECK(bind(s, (struct sockaddr *)&a, sizeof a) == 0, "bind to port 5080: %s", strerror(errno));
    close(s);
}

static pid_t root_sleeper;

static void user_signals(void)
{
    CHECK(FAILS(kill(root_sleeper, SIGTERM), EPERM), "signal to a process of root");
    CHECK(FAILS(kill(root_sleeper, 0), EPERM), "probe of a process of root");
    CHECK(FAILS(kill(1, SIGTERM), EPERM), "signal to init");
    pid_t own = fork();
    if (own == 0) {
        wait_forever();
    }
    CHECK(kill(own, SIGTERM) == 0, "signal to an own process: %s", strerror(errno));
    int status;
    waitpid(own, &status, 0);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM, "own process not terminated");
    CHECK(kill(-1, 0) == 0, "kill(-1, 0) finds the own processes");

    /* A pseudo terminal belongs to the user who opened the master. */
    int m = open("/dev/ptmx", O_RDWR);
    CHECK(m >= 0, "open /dev/ptmx: %s", strerror(errno));
    if (m >= 0) {
        int n = -1;
        ioctl(m, TIOCGPTN, &n);
        char name[32];
        snprintf(name, sizeof name, "/dev/pts%d", n);
        struct stat st;
        CHECK(stat(name, &st) == 0 && st.st_uid == 1000 && (st.st_mode & 07777) == 0620,
              "%s: uid %u mode %o", name, st.st_uid, st.st_mode & 07777);
        int sl = open(name, O_RDWR);
        CHECK(sl >= 0, "open of the own slave: %s", strerror(errno));
        if (sl >= 0)
            close(sl);
        close(m);
        CHECK(stat(name, &st) == 0 && st.st_uid == 0 && (st.st_mode & 07777) == 0600,
              "%s after close: uid %u mode %o", name, st.st_uid, st.st_mode & 07777);
    }
}

static void root_checks(void)
{
    int fd = open("/tmp/perm/open/mine", O_RDWR);
    CHECK(fd >= 0, "root cannot open a 0444 file of uid 1000: %s", strerror(errno));
    if (fd >= 0)
        close(fd);
    struct stat st;
    CHECK(stat("/tmp/perm/private/f", &st) == 0, "root cannot search a 0700 directory");
    char *noexec[] = { "/tmp/perm/id-noexec", NULL };
    char out[64];
    CHECK(run_capture(noexec, out, sizeof out) == 127, "root executed a file without execute bits");
}

static int secure_mode(void)
{
    uid_t r, e, s;
    getresuid(&r, &e, &s);
    printf("secure %ld ruid %u euid %u suid %u\n", aux_value(AT_SECURE_TYPE), r, e, s);
    return 0;
}

static int access_mode(const char *path)
{
    int a = access(path, R_OK);
    const char *what = a == 0 ? "0" : errno == EACCES ? "EACCES" : strerror(errno);
    int ea = faccessat(AT_FDCWD, path, R_OK, AT_EACCESS);
    int fd = open(path, O_RDONLY);
    printf("access %s eaccess %d open %d\n", what, ea, fd >= 0 ? 0 : -1);
    if (fd >= 0)
        close(fd);
    return 0;
}

int main(int argc, char **argv, char **envp)
{
    saved_envp = envp;
    if (argc == 2 && strcmp(argv[1], "--secure") == 0)
        return secure_mode();
    if (argc == 3 && strcmp(argv[1], "--access") == 0)
        return access_mode(argv[2]);
    setup();
    root_sleeper = fork();
    if (root_sleeper == 0) {
        wait_forever();
    }
    as_user("files", user_files);
    as_user("exec", user_exec);
    as_user("privileged", user_privileged);
    as_user("signals", user_signals);
    root_checks();
    kill(root_sleeper, SIGKILL);
    waitpid(root_sleeper, NULL, 0);
    printf("permtest: %d failures\n", failures);
    return failures ? 1 : 0;
}
