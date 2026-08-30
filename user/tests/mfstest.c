/* M13 user test: the disk filesystem through system calls and coreutils
 * style operations. Exits 0 on success. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static int count_entries(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d)
        return -1;
    int n = 0;
    while (readdir(d))
        n++;
    closedir(d);
    return n;
}

int main(void)
{
    printf("mfstest: pid %d\n", getpid());
    struct stat st;
    CHECK(mkdir("/work", 0755) == 0, "mkdir /work: %s", strerror(errno));
    CHECK(chdir("/work") == 0, "chdir");

    /* Create, append, read back. */
    FILE *f = fopen("notes.txt", "w");
    CHECK(f != NULL, "fopen w");
    for (int i = 0; i < 1000; i++)
        fprintf(f, "line %d\n", i);
    CHECK(fclose(f) == 0, "fclose");
    CHECK(stat("notes.txt", &st) == 0 && st.st_size == 8890, "size %ld", (long)st.st_size);
    f = fopen("notes.txt", "a");
    CHECK(f && fputs("appended\n", f) == 0 && fclose(f) == 0, "append");
    f = fopen("notes.txt", "r");
    char line[64];
    int lines = 0;
    while (fgets(line, sizeof line, f))
        lines++;
    CHECK(lines == 1001 && strcmp(line, "appended\n") == 0, "lines %d last '%s'", lines, line);
    fclose(f);

    /* Overwrite in the middle with pwrite style seeking. */
    int fd = open("notes.txt", O_RDWR);
    CHECK(fd >= 0 && lseek(fd, 5, SEEK_SET) == 5 && write(fd, "X", 1) == 1, "seek write");
    char buf[16];
    CHECK(lseek(fd, 0, SEEK_SET) == 0 && read(fd, buf, 8) == 8 && memcmp(buf, "line X\nl", 8) == 0, "overwrite");
    close(fd);

    /* Rename, link, unlink, rmdir. */
    CHECK(rename("notes.txt", "renamed.txt") == 0 && stat("notes.txt", &st) < 0, "rename");
    CHECK(link("renamed.txt", "hard.txt") == 0 && stat("hard.txt", &st) == 0 && st.st_nlink == 2, "link");
    CHECK(unlink("renamed.txt") == 0 && stat("hard.txt", &st) == 0 && st.st_nlink == 1, "unlink one link");
    CHECK(count_entries("/work") == 3, "entries %d", count_entries("/work"));
    CHECK(mkdir("sub", 0755) == 0 && rmdir("/work") < 0 && errno == ENOTEMPTY, "rmdir non empty");
    CHECK(rename("hard.txt", "sub/file.txt") == 0 && stat("sub/file.txt", &st) == 0, "move into subdir");
    CHECK(unlink("sub/file.txt") == 0 && rmdir("sub") == 0, "cleanup sub");
    CHECK(open("/work/x/y", O_CREAT | O_WRONLY, 0644) < 0 && errno == ENOENT, "create in missing dir");
    CHECK(open("/work", O_WRONLY) < 0 && errno == EISDIR, "open dir for write");

    /* Coreutils on the disk: cp, cat via the shell. */
    pid_t pid = fork();
    if (pid == 0) {
        char *const args[] = { "sh", "-c", "cp /etc/motd /work/copy.txt", NULL };
        execvp("sh", args);
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    CHECK(WEXITSTATUS(status) == 0 && stat("/work/copy.txt", &st) == 0 && st.st_size == 53, "cp via shell: %ld",
          (long)st.st_size);
    pid = fork();
    if (pid == 0) {
        char *const args[] = { "sh", "-c", "cat /work/copy.txt | wc -l > /work/count.txt", NULL };
        execvp("sh", args);
        _exit(127);
    }
    waitpid(pid, &status, 0);
    f = fopen("/work/count.txt", "r");
    CHECK(f && fgets(line, sizeof line, f) && atoi(line) == 2, "pipeline into file: '%s'", f ? line : "");
    if (f)
        fclose(f);
    CHECK(unlink("/work/copy.txt") == 0 && unlink("/work/count.txt") == 0, "unlink copies");
    CHECK(chdir("/") == 0 && rmdir("/work") == 0, "rmdir /work");
    sync();

    printf("mfstest: %d failures\n", failures);
    return failures ? 1 : 0;
}
