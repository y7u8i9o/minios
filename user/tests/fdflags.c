/* M23: close on exec and descriptor flags. Re-executes itself with
 * "child <kept> <closed>". */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/mman.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("fdflags: FAIL " __VA_ARGS__); printf(" (errno %d)\n", errno); } } while (0)

int main(int argc, char **argv)
{
    if (argc == 4 && strcmp(argv[1], "child") == 0) {
        int kept = atoi(argv[2]), closed = atoi(argv[3]);
        errno = 0;
        int r = fcntl(closed, F_GETFD);
        if (r != -1 || errno != EBADF) {
            printf("fdflags: FAIL close on exec descriptor %d survived exec\n", closed);
            return 1;
        }
        char c;
        if (read(kept, &c, 1) != 1 || c == 0) {
            printf("fdflags: FAIL kept descriptor %d unreadable\n", kept);
            return 1;
        }
        printf("fdflags: child ok\n");
        return 0;
    }
    int p1[2], p2[2];
    CHECK(pipe2(p1, O_CLOEXEC) == 0, "pipe2 with O_CLOEXEC");
    CHECK(pipe2(p2, O_NONBLOCK) == 0, "pipe2 with O_NONBLOCK");
    CHECK(fcntl(p1[0], F_GETFD) == FD_CLOEXEC, "close on exec flag set");
    CHECK(fcntl(p2[0], F_GETFD) == 0, "close on exec flag clear");
    char c;
    errno = 0;
    CHECK(read(p2[0], &c, 1) == -1 && errno == EAGAIN, "nonblocking pipe read gives EAGAIN");
    int dup_cloexec = fcntl(p2[1], F_DUPFD_CLOEXEC, 10);
    CHECK(dup_cloexec >= 10 && fcntl(dup_cloexec, F_GETFD) == FD_CLOEXEC, "F_DUPFD_CLOEXEC");
    int plain = fcntl(p2[1], F_DUPFD, 20);
    CHECK(plain >= 20 && fcntl(plain, F_GETFD) == 0, "F_DUPFD");
    CHECK(fcntl(plain, F_SETFD, FD_CLOEXEC) == 0 && fcntl(plain, F_GETFD) == FD_CLOEXEC, "F_SETFD");
    int m = memfd_create("m", MFD_CLOEXEC);
    CHECK(m >= 0 && fcntl(m, F_GETFD) == FD_CLOEXEC, "memfd_create MFD_CLOEXEC");
    int o = open("/etc/launcher", O_RDONLY | O_CLOEXEC);
    CHECK(o >= 0 && fcntl(o, F_GETFD) == FD_CLOEXEC, "open with O_CLOEXEC");
    CHECK(write(p2[1], "x", 1) == 1, "write for the child");
    pid_t pid = fork();
    if (pid == 0) {
        char kept[8], closed[8];
        snprintf(kept, sizeof kept, "%d", p2[0]);
        snprintf(closed, sizeof closed, "%d", p1[0]);
        char *const args[] = { "fdflags", "child", kept, closed, NULL };
        execvp("/bin/fdflags", args);
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    CHECK(status == 0, "child status 0x%x", status);
    CHECK(fcntl(p1[0], F_GETFD) == FD_CLOEXEC, "parent keeps its descriptor after the child's exec");
    printf("fdflags: %d failures\n", failures);
    return failures ? 1 : 0;
}
