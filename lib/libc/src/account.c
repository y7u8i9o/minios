/* Editing the account databases and checking passwords
 * (docs/design/users.md). */
#include <minios/account.h>
#include <minios/init.h>
#include <minios/sha2.h>
#include <shadow.h>
#include <grp.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <termios.h>
#include <time.h>

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

static int copy_file(const char *from, const char *to, mode_t mode)
{
    int in = open(from, O_RDONLY | O_CLOEXEC);
    if (in < 0)
        return -1;
    int out = open(to, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode & 07777);
    if (out < 0) {
        int e = errno;
        close(in);
        errno = e;
        return e == EEXIST ? 0 : -1;
    }
    char buf[16384];
    ssize_t n;
    int r = 0;
    while (r == 0 && (n = read(in, buf, sizeof buf)) > 0) {
        for (ssize_t done = 0; done < n;) {
            ssize_t w = write(out, buf + done, (size_t)(n - done));
            if (w <= 0) {
                r = -1;
                break;
            }
            done += w;
        }
    }
    if (n < 0)
        r = -1;
    close(in);
    close(out);
    return r;
}

int account_copy_tree(const char *from, const char *to)
{
    DIR *d = opendir(from);
    if (d == NULL)
        return -1;
    struct dirent *e;
    int r = 0;
    while (r == 0 && (e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char src[PATH_MAX], dst[PATH_MAX];
        snprintf(src, sizeof src, "%s/%s", from, e->d_name);
        snprintf(dst, sizeof dst, "%s/%s", to, e->d_name);
        struct stat st;
        if (lstat(src, &st) < 0) {
            r = -1;
        } else if (S_ISLNK(st.st_mode)) {
            /* A link is copied as a link, never through its target. */
            char target[256];
            ssize_t n = readlink(src, target, sizeof target - 1);
            if (n < 0) {
                r = -1;
            } else {
                target[n] = '\0';
                if (symlink(target, dst) < 0 && errno != EEXIST)
                    r = -1;
            }
        } else if (S_ISDIR(st.st_mode)) {
            if (mkdir(dst, st.st_mode & 07777) < 0 && errno != EEXIST)
                r = -1;
            else
                r = account_copy_tree(src, dst);
        } else if (S_ISREG(st.st_mode)) {
            r = copy_file(src, dst, st.st_mode);
        }
    }
    closedir(d);
    return r;
}

int account_chown_tree(const char *path, unsigned uid, unsigned gid)
{
    struct stat st;
    if (lstat(path, &st) < 0 || lchown(path, uid, gid) < 0)
        return -1;
    if (!S_ISDIR(st.st_mode))
        return 0;
    DIR *d = opendir(path);
    if (d == NULL)
        return -1;
    struct dirent *e;
    int r = 0;
    while (r == 0 && (e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char sub[PATH_MAX];
        snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
        r = account_chown_tree(sub, uid, gid);
    }
    closedir(d);
    return r;
}

int account_make_home(const char *dir, unsigned uid, unsigned gid)
{
    struct stat st;
    if (stat(dir, &st) == 0)
        return 0;
    if (mkdir(dir, 0700) < 0)
        return -1;
    /* The mode is set apart from mkdir, which the umask would narrow. */
    if (chmod(dir, 0700) < 0)
        return -1;
    if (account_copy_tree(ACCOUNT_SKEL, dir) < 0 && errno != ENOENT)
        return -1;
    return account_chown_tree(dir, uid, gid);
}

int account_read_password(const char *prompt, char *buf, size_t size)
{
    struct termios saved, quiet;
    int tty = isatty(0) && tcgetattr(0, &saved) == 0;
    if (tty) {
        fputs(prompt, stderr);
        fflush(stderr);
        quiet = saved;
        quiet.c_lflag &= ~(uint32_t)ECHO;
        tcsetattr(0, TCSANOW, &quiet);
    }
    size_t n = 0;
    int c = 0;
    while (n + 1 < size && read(0, &buf[n], 1) == 1 && (c = (unsigned char)buf[n]) != '\n')
        n++;
    int eof = n == 0 && c != '\n';
    buf[n] = '\0';
    if (tty) {
        tcsetattr(0, TCSANOW, &saved);
        fputc('\n', stderr);
    }
    return eof ? -1 : 0;
}

int account_session(int uid)
{
    char request[32];
    if (uid >= 0)
        snprintf(request, sizeof request, "session %d", uid);
    else
        snprintf(request, sizeof request, "session -");
    char reply[64];
    long n = init_request(request, reply, sizeof reply);
    if (n < 2 || strncmp(reply, "ok", 2) != 0) {
        errno = EPERM;
        return -1;
    }
    return 0;
}

int account_become(const char *name, unsigned uid, unsigned gid)
{
    if (initgroups(name, gid) < 0 || setgid(gid) < 0 || setuid(uid) < 0)
        return -1;
    return 0;
}

long account_today(void)
{
    return (long)(time(NULL) / 86400);
}

/* A numeric field of /etc/shadow, empty for -1. */
static const char *shadow_field(long v, char *buf, size_t size)
{
    if (v < 0)
        buf[0] = '\0';
    else
        snprintf(buf, size, "%ld", v);
    return buf;
}

int account_set_hash(const char *name, const char *hash)
{
    struct spwd *sp = getspnam(name);
    char line[512], a[24], b[24], c[24], d[24], e[24], f[24];
    if (sp)
        snprintf(line, sizeof line, "%s:%s:%ld:%s:%s:%s:%s:%s:%s", name, hash, account_today(),
                 shadow_field(sp->sp_min, a, sizeof a), shadow_field(sp->sp_max, b, sizeof b),
                 shadow_field(sp->sp_warn, c, sizeof c), shadow_field(sp->sp_inact, d, sizeof d),
                 shadow_field(sp->sp_expire, e, sizeof e),
                 sp->sp_flag == (unsigned long)-1 ? "" : shadow_field((long)sp->sp_flag, f, sizeof f));
    else
        snprintf(line, sizeof line, "%s:%s:%ld::::::", name, hash, account_today());
    return account_replace(ACCOUNT_SHADOW, name, line);
}

int account_set_password(const char *name, const char *password)
{
    char hash[128];
    if (!password[0]) {
        errno = EINVAL;
        return -1;
    }
    if (account_hash(password, hash, sizeof hash) < 0)
        return -1;
    return account_set_hash(name, hash);
}

/* A setting of another method, or an empty one, gives "*0", which matches
 * no stored hash, as the crypt of libxcrypt does. */
char *crypt(const char *key, const char *salt)
{
    static char out[128];
    if (!sha256_crypt(key, salt, out, sizeof out)) {
        errno = EINVAL;
        strcpy(out, salt[0] == '*' && salt[1] == '0' ? "*1" : "*0");
    }
    return out;
}
