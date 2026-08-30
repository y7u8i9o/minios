/* M11 test: open, read, lseek, dup, stat, getdents, pipes, redirection of
 * descriptors and error paths on the read only root. Exits 0 on success. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static int has_entry(const char *dir, const char *name, int type)
{
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    struct dirent *e;
    int found = 0;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, name) == 0 && e->d_type == type)
            found = 1;
    }
    closedir(d);
    return found;
}

int main(int argc, char **argv)
{
    printf("fstest: pid %d\n", getpid());

    /* Regular file on the initrd. */
    int fd = open("/etc/motd", O_RDONLY);
    CHECK(fd >= 3, "open /etc/motd: %s", strerror(errno));
    char buf[128];
    ssize_t n = read(fd, buf, 7);
    CHECK(n == 7 && memcmp(buf, "Welcome", 7) == 0, "read start");
    struct stat st;
    CHECK(fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 20, "fstat motd");
    CHECK(lseek(fd, 0, SEEK_CUR) == 7, "lseek cur");
    CHECK(lseek(fd, -6, SEEK_END) == st.st_size - 6, "lseek end");
    n = read(fd, buf, sizeof buf);
    CHECK(n == 6 && memcmp(buf, "itrd.\n", 6) == 0, "read tail: %ld", (long)n);
    CHECK(read(fd, buf, sizeof buf) == 0, "eof");
    CHECK(lseek(fd, 0, SEEK_SET) == 0, "lseek set");
    int fd2 = dup(fd);
    CHECK(fd2 > fd, "dup");
    CHECK(read(fd2, buf, 3) == 3 && lseek(fd, 0, SEEK_CUR) == 3, "dup shares offset");
    CHECK(write(fd, "x", 1) < 0 && errno == EBADF, "write to O_RDONLY");
    CHECK(close(fd) == 0 && close(fd2) == 0 && close(fd2) < 0 && errno == EBADF, "close");

    /* stat and directories. */
    CHECK(stat("/bin/sh", &st) == 0 && S_ISREG(st.st_mode), "stat /bin/sh");
    CHECK(stat("/bin", &st) == 0 && S_ISDIR(st.st_mode), "stat /bin");
    CHECK(stat("/dev", &st) == 0 && S_ISDIR(st.st_mode), "stat /dev");
    CHECK(stat("/dev/null", &st) == 0 && S_ISCHR(st.st_mode), "stat /dev/null");
    CHECK(stat("/nosuch", &st) < 0 && errno == ENOENT, "stat missing");
    CHECK(stat("/etc/motd/x", &st) < 0 && errno == ENOTDIR, "stat through file");
    CHECK(has_entry("/bin", "fstest", DT_REG), "readdir /bin");
    CHECK(has_entry("/", "dev", DT_DIR), "readdir / has dev");
    CHECK(has_entry("/dev", "console", DT_CHR), "readdir /dev has console");
    CHECK(has_entry("/dev", "zero", DT_CHR), "readdir /dev has zero");
    CHECK(open("/bin", O_WRONLY) < 0 && errno == EISDIR, "open dir for writing");
    CHECK(open("/etc/motd", O_RDONLY | O_DIRECTORY) < 0 && errno == ENOTDIR, "O_DIRECTORY");
    fd = open("/bin", O_RDONLY);
    CHECK(fd >= 0 && read(fd, buf, 1) < 0 && errno == EISDIR, "read dir");
    close(fd);

    /* The initrd is read only (mounted on /initrd when the disk is root). */
    const char *ro = stat("/initrd/etc/motd", &st) == 0 ? "/initrd" : "";
    char p1[64], p2[64];
    snprintf(p1, sizeof p1, "%s/newdir", ro);
    CHECK(mkdir(p1, 0755) < 0 && errno == EROFS, "mkdir on initrd");
    snprintf(p1, sizeof p1, "%s/etc/motd", ro);
    CHECK(unlink(p1) < 0 && errno == EROFS, "unlink on initrd");
    snprintf(p2, sizeof p2, "%s/etc/new", ro);
    CHECK(open(p2, O_WRONLY | O_CREAT, 0644) < 0 && errno == EROFS, "creat on initrd");
    snprintf(p2, sizeof p2, "%s/etc/x", ro);
    CHECK(rename(p1, p2) < 0 && errno == EROFS, "rename on initrd");

    /* Relative paths and cwd. */
    CHECK(chdir("/etc") == 0, "chdir /etc");
    fd = open("motd", O_RDONLY);
    CHECK(fd >= 0, "relative open");
    close(fd);
    fd = open("../bin/../etc/./motd", O_RDONLY);
    CHECK(fd >= 0, "dot components");
    close(fd);
    CHECK(chdir("/") == 0, "chdir /");

    /* Devices. */
    fd = open("/dev/zero", O_RDONLY);
    memset(buf, 1, sizeof buf);
    CHECK(fd >= 0 && read(fd, buf, sizeof buf) == (ssize_t)sizeof buf && buf[0] == 0 && buf[127] == 0, "/dev/zero");
    close(fd);
    fd = open("/dev/null", O_RDWR);
    CHECK(fd >= 0 && write(fd, "abc", 3) == 3 && read(fd, buf, 10) == 0, "/dev/null");
    close(fd);

    /* Pipes: parent writes, child reads, and EOF when the writer closes. */
    int p[2];
    CHECK(pipe(p) == 0, "pipe");
    pid_t pid = fork();
    if (pid == 0) {
        close(p[1]);
        size_t total = 0;
        while ((n = read(p[0], buf, sizeof buf)) > 0)
            total += (size_t)n;
        _exit(total == 20000 ? 0 : 1);
    }
    close(p[0]);
    char chunk[1000];
    memset(chunk, 'a', sizeof chunk);
    for (int i = 0; i < 20; i++)
        CHECK(write(p[1], chunk, sizeof chunk) == (ssize_t)sizeof chunk, "pipe write %d", i);
    close(p[1]);
    int status;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "pipe reader status 0x%x", status);

    /* Writing to a pipe without readers fails with EPIPE once SIGPIPE is ignored. */
    signal(SIGPIPE, SIG_IGN);
    CHECK(pipe(p) == 0, "pipe 2");
    close(p[0]);
    CHECK(write(p[1], "x", 1) < 0 && errno == EPIPE, "EPIPE");
    close(p[1]);
    CHECK(lseek(p[1], 0, SEEK_SET) < 0 && errno == EBADF, "lseek closed pipe");

    /* dup2 based redirection into a pipe, read back by the parent. */
    CHECK(pipe(p) == 0, "pipe 3");
    pid = fork();
    if (pid == 0) {
        dup2(p[1], 1);
        close(p[0]);
        close(p[1]);
        char *const args[] = { "echo", "through", "a", "pipe", NULL };
        execvp("echo", args);
        _exit(127);
    }
    close(p[1]);
    size_t got = 0;
    while ((n = read(p[0], buf + got, sizeof buf - got)) > 0)
        got += (size_t)n;
    buf[got] = '\0';
    close(p[0]);
    waitpid(pid, &status, 0);
    CHECK(strcmp(buf, "through a pipe\n") == 0, "pipe output '%s'", buf);
    CHECK(lseek(p[0], 0, SEEK_SET) < 0 && errno == EBADF, "closed pipe fd");

    /* stdio streams over files. */
    FILE *f = fopen("/etc/motd", "r");
    CHECK(f != NULL, "fopen");
    CHECK(fgets(buf, sizeof buf, f) && strcmp(buf, "Welcome to minios.\n") == 0, "fgets '%s'", buf);
    CHECK(fseek(f, 0, SEEK_SET) == 0 && fgetc(f) == 'W', "fseek");
    CHECK(fclose(f) == 0, "fclose");
    CHECK(fopen("/nosuch", "r") == NULL && errno == ENOENT, "fopen missing");

    /* Descriptors survive exec and fork. */
    fd = open("/etc/motd", O_RDONLY);
    CHECK(lseek(fd, 8, SEEK_SET) == 8, "seek before fork");
    pid = fork();
    if (pid == 0) {
        n = read(fd, buf, 2);
        _exit(n == 2 && buf[0] == 't' && buf[1] == 'o' ? 0 : 1);
    }
    waitpid(pid, &status, 0);
    CHECK(WEXITSTATUS(status) == 0, "child inherited fd");
    CHECK(lseek(fd, 0, SEEK_CUR) == 10, "offset shared after child read");
    close(fd);

    /* Descriptor exhaustion. */
    int fds[128], count = 0;
    while (count < 128 && (fds[count] = open("/dev/null", O_RDONLY)) >= 0)
        count++;
    CHECK(errno == EMFILE && count == OPEN_MAX - 3, "EMFILE after %d opens", count);
    for (int i = 0; i < count; i++)
        close(fds[i]);

    printf("fstest: %d failures\n", failures);
    return failures ? 1 : 0;
}
