/* Editing the account databases and checking passwords
 * (docs/design/users.md). */
#include <minios/account.h>
#include <minios/sha2.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

int account_replace(const char *path, const char *name, const char *line)
{
    char target[PATH_MAX], tmp[PATH_MAX + 8];
    if (!realpath(path, target))
        return -1;
    snprintf(tmp, sizeof tmp, "%s.new", target);
    struct stat st;
    if (stat(target, &st) < 0)
        return -1;
    FILE *in = fopen(target, "r");
    if (!in)
        return -1;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, st.st_mode & 07777);
    FILE *out = fd >= 0 ? fdopen(fd, "w") : NULL;
    if (!out) {
        int e = errno;
        if (fd >= 0)
            close(fd);
        fclose(in);
        errno = e;
        return -1;
    }
    size_t len = strlen(name);
    bool done = false;
    char buf[1024];
    while (fgets(buf, sizeof buf, in)) {
        if (strncmp(buf, name, len) == 0 && buf[len] == ':') {
            if (line && !done)
                fprintf(out, "%s\n", line);
            done = true;
            continue;
        }
        fputs(buf, out);
        if (!strchr(buf, '\n'))
            fputc('\n', out);
    }
    if (line && !done)
        fprintf(out, "%s\n", line);
    fclose(in);
    int failed = ferror(out);
    if (fflush(out) != 0)
        failed = 1;
    /* The new file takes the owner of the old one, which matters when root
     * rewrites a file another account owns. */
    if (!failed && fchown(fileno(out), st.st_uid, st.st_gid) < 0)
        failed = 1;
    int e = errno;
    fclose(out);
    if (failed || rename(tmp, target) < 0) {
        e = failed ? e : errno;
        unlink(tmp);
        errno = e ? e : EIO;
        return -1;
    }
    return 0;
}

int account_hash(const char *password, char *out, size_t size)
{
    static const char alphabet[] = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    unsigned char random[16];
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, random, sizeof random);
    close(fd);
    if (n != (ssize_t)sizeof random) {
        errno = EIO;
        return -1;
    }
    char setting[24] = "$5$";
    for (int i = 0; i < 16; i++)
        setting[3 + i] = alphabet[random[i] & 0x3f];
    setting[19] = '\0';
    if (!sha256_crypt(password, setting, out, size)) {
        errno = ERANGE;
        return -1;
    }
    return 0;
}

bool account_check(const char *password, const char *hash)
{
    if (!hash[0])
        return !password[0];
    if (hash[0] == '!' || hash[0] == '*')
        return false;
    char out[128];
    if (!sha256_crypt(password, hash, out, sizeof out))
        return false;
    /* Compare every byte, independent of where the first difference is. */
    size_t a = strlen(out), b = strlen(hash);
    unsigned diff = (unsigned)(a ^ b);
    for (size_t i = 0; i < a && i < b; i++)
        diff |= (unsigned char)(out[i] ^ hash[i]);
    memset(out, 0, sizeof out);
    return diff == 0;
}

bool account_name_valid(const char *name)
{
    if (!(name[0] >= 'a' && name[0] <= 'z'))
        return false;
    size_t n = 1;
    for (; name[n]; n++) {
        char c = name[n];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return false;
    }
    return n <= 32;
}

char *crypt(const char *key, const char *salt)
{
    static char out[128];
    if (!sha256_crypt(key, salt, out, sizeof out)) {
        errno = EINVAL;
        return NULL;
    }
    return out;
}
