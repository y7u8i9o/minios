/* U3 test: the conversion of a data volume from the single user layout.
 * Started as root by the fs_migrate case, it builds a directory in the old
 * layout below /tmp, runs fsinit -m on it and checks that the old home
 * moved into user/ with uid 1000, that the package prefix remained with root
 * and gained the account databases, and that a second run changes
 * nothing. It also checks that fsinit adds the group wheel to a group file
 * without it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define VOL "/tmp/oldvol"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static void write_file(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        write(fd, text, strlen(text));
        close(fd);
    }
}

static int run_fsinit(void)
{
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        execl("/bin/fsinit", "fsinit", "-m", VOL, (char *)NULL);
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* path exists with the given owner, and with the mode unless mode is 0. */
static int has(const char *path, unsigned uid, unsigned mode)
{
    struct stat st;
    if (lstat(path, &st) < 0)
        return 0;
    return st.st_uid == uid && (!mode || (st.st_mode & 07777) == mode);
}

int main(void)
{
    mkdir(VOL, 0755);
    mkdir(VOL "/desktop", 0755);
    write_file(VOL "/desktop/readme.txt", "hello\n");
    write_file(VOL "/.shrc", "# shell\n");
    mkdir(VOL "/.config", 0755);
    write_file(VOL "/.config/desktop.conf", "lang=fr_FR\n");
    /* The old home contains an entry named user, which must not collide. */
    write_file(VOL "/user", "a file called user\n");
    symlink("desktop/readme.txt", VOL "/link");
    mkdir(VOL "/.local", 0755);
    mkdir(VOL "/.local/bin", 0755);
    write_file(VOL "/.local/bin/tool", "#!/bin/sh\n");
    mkdir(VOL "/lost+found", 0700);

    CHECK(run_fsinit() == 0, "fsinit -m failed");
    CHECK(has(VOL "/user", 1000, 0700), "user/ is not the home of uid 1000");
    CHECK(has(VOL "/user/desktop/readme.txt", 1000, 0), "desktop/readme.txt did not move");
    CHECK(has(VOL "/user/.shrc", 1000, 0), ".shrc did not move");
    CHECK(has(VOL "/user/.config/desktop.conf", 1000, 0), ".config did not move");
    CHECK(has(VOL "/user/user", 1000, 0), "the entry named user did not move");
    CHECK(has(VOL "/user/link", 1000, 0), "the link did not move or was followed");
    CHECK(has(VOL "/.local/bin/tool", 0, 0), "the package prefix moved");
    CHECK(has(VOL "/.local", 0, 0755), "the package prefix is not root's");
    CHECK(has(VOL "/.local/etc/passwd", 0, 0644), "no account database");
    CHECK(has(VOL "/.local/etc/shadow", 0, 0600), "no password database with mode 0600");
    CHECK(has(VOL "/lost+found", 0, 0), "lost+found moved");
    CHECK(has(VOL "/.layout", 0, 0), "no layout marker");
    CHECK(access(VOL "/.migrate-user", F_OK) < 0, "the temporary directory remains");
    CHECK(access(VOL "/desktop", F_OK) < 0, "desktop/ is still at the top");

    /* A converted volume is left alone. */
    write_file(VOL "/later", "after the conversion\n");
    CHECK(run_fsinit() == 0, "second fsinit -m failed");
    CHECK(has(VOL "/later", 0, 0), "a second run moved a file");
    CHECK(access(VOL "/user/later", F_OK) < 0, "a second run converted again");

    /* A volume made before U5 has a group file without wheel. fsinit adds
     * wheel with the account of uid 1000, and a second run adds nothing. */
    write_file(VOL "/.local/etc/group", "root:x:0:\nuser:x:1000:\n");
    CHECK(run_fsinit() == 0, "fsinit -m on a volume without wheel failed");
    char group[256] = "";
    int fd = open(VOL "/.local/etc/group", O_RDONLY);
    if (fd >= 0) {
        long n = read(fd, group, sizeof group - 1);
        group[n > 0 ? n : 0] = '\0';
        close(fd);
    }
    CHECK(strstr(group, "\nwheel:x:10:user\n") != NULL, "no wheel with user: %s", group);
    CHECK(run_fsinit() == 0, "second fsinit -m failed");
    fd = open(VOL "/.local/etc/group", O_RDONLY);
    char again[256] = "";
    if (fd >= 0) {
        long n = read(fd, again, sizeof again - 1);
        again[n > 0 ? n : 0] = '\0';
        close(fd);
    }
    CHECK(strcmp(group, again) == 0, "a second run changed the group file: %s", again);

    printf("migratetest: %d failures\n", failures);
    return failures ? 1 : 0;
}
