/* M36 user test: a FAT16 image on vdb mounted with mount(2), used through
 * libc: directory listing with long names, reading, writing, renaming
 * and removing, then unmounted. Exits 0 on success. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/mount.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static int has_entry(const char *dir, const char *name)
{
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    struct dirent *e;
    int found = 0;
    while ((e = readdir(d)))
        if (strcmp(e->d_name, name) == 0)
            found = 1;
    closedir(d);
    return found;
}

int main(void)
{
    CHECK(mkdir("/mnt", 0755) == 0, "mkdir /mnt: %s", strerror(errno));
    CHECK(mount("vdb", "/mnt", "fat") == 0, "mount: %s", strerror(errno));
    CHECK(has_entry("/mnt", "Long Directory Name"), "long directory name listed");
    CHECK(has_entry("/mnt", "readme.txt"), "short name listed in lower case");
    CHECK(has_entry("/mnt/Long Directory Name/nested", "big data file.bin"), "nested long name");

    FILE *f = fopen("/mnt/README.TXT", "r");
    CHECK(f != NULL, "fopen README.TXT");
    char line[64] = "";
    if (f) {
        CHECK(fgets(line, sizeof line, f) && strcmp(line, "hello\n") == 0, "readme content \"%s\"", line);
        fclose(f);
    }
    struct stat st;
    CHECK(stat("/mnt/Long Directory Name/nested/big data file.bin", &st) == 0 && st.st_size == 20000,
          "stat of the big file");
    CHECK(stat("/mnt/Long Directory Name", &st) == 0 && S_ISDIR(st.st_mode), "stat of a directory");

    f = fopen("/mnt/Notes from user space.txt", "w");
    CHECK(f != NULL, "create long name");
    if (f) {
        for (int i = 0; i < 500; i++)
            fprintf(f, "line %d\n", i);
        CHECK(fclose(f) == 0, "fclose");
    }
    f = fopen("/mnt/notes FROM user SPACE.txt", "r");
    CHECK(f != NULL, "reopen with other case");
    if (f) {
        int lines = 0;
        while (fgets(line, sizeof line, f))
            lines++;
        CHECK(lines == 500, "%d lines read", lines);
        fclose(f);
    }
    CHECK(rename("/mnt/Notes from user space.txt", "/mnt/Long Directory Name/moved notes.txt") == 0,
          "rename: %s", strerror(errno));
    CHECK(!has_entry("/mnt", "Notes from user space.txt"), "old name gone");
    CHECK(has_entry("/mnt/Long Directory Name", "moved notes.txt"), "new name listed");
    CHECK(unlink("/mnt/Long Directory Name/moved notes.txt") == 0, "unlink");
    CHECK(mkdir("/mnt/Sub Directory", 0755) == 0, "mkdir long");
    CHECK(rmdir("/mnt/Sub Directory") == 0, "rmdir long");
    CHECK(unlink("/mnt/Long Directory Name") < 0 && errno == EISDIR, "unlink directory");
    sync();
    CHECK(umount("/mnt") == 0, "umount: %s", strerror(errno));
    CHECK(!has_entry("/mnt", "readme.txt"), "unmounted");
    printf("fattest: %d failures\n", failures);
    return failures ? 1 : 0;
}
