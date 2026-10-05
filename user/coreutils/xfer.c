/* xfer: file transfer between the guest and the host, or any two machines.
 *
 *   xfer put [-h HOST] [-p PORT] PATH...       send files or directories
 *   xfer get [-h HOST] [-p PORT] NAME... [-o DIR]  fetch files or directories
 *   xfer ls  [-h HOST] [-p PORT] [DIR]         list the server directory
 *   xfer serve [-p PORT] [DIR]                 serve DIR (default .)
 *
 * The host defaults to 10.0.2.2, the host as seen from QEMU user
 * networking.  The client port defaults to 9101 and the server port to
 * 9100: make run forwards host port 9100 to the guest, so a server of the
 * guest listens on 9100 and a server of the host on 9101.  In the guest
 * "xfer put file" therefore sends to a host running "tools/xfer.py serve".
 * Every transfer carries a CRC32 that both ends check.  Directories are
 * sent file by file with their relative paths, and each directory, empty
 * ones included, through a MKDIR request; the server accepts only relative
 * paths without "..".  The server writes an upload to a temporary file and
 * renames it over the target only after a complete and correct transfer,
 * so a failed upload leaves an existing file intact.  A connection that
 * makes no progress for 30 seconds is dropped.  The server forks one child
 * per client.  See xfer(1). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <minios/crc32.h>

#define DEFAULT_HOST "10.0.2.2"
#define DEFAULT_PORT "9101"             /* the server of the host */
#define DEFAULT_SERVE_PORT "9100"       /* the port that make run forwards to the guest */
#define PART_SUFFIX ".xfer-part"
#define STALL_MS 30000
#define CHUNK 65536

static const char *host = DEFAULT_HOST, *port = NULL;     /* NULL: the default of the command */
static const char *outdir = ".";
static int failures;

/* ---- bounded socket I/O ---- */
static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static int ready(int fd, short events)
{
    struct pollfd pfd = {.fd = fd, .events = events};
    if (poll(&pfd, 1, STALL_MS) != 1) {
        errno = ETIMEDOUT;
        return -1;
    }
    return 0;
}

static int send_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    while (n) {
        if (ready(fd, POLLOUT) < 0)
            return -1;
        ssize_t r = send(fd, p, n, MSG_NOSIGNAL);
        if (r <= 0)
            return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

static int read_line(int fd, char *line, size_t size)
{
    size_t n = 0;
    while (n + 1 < size) {
        char c;
        if (ready(fd, POLLIN) < 0)
            return -1;
        ssize_t r = read(fd, &c, 1);
        if (r <= 0)
            return -1;
        if (c == '\n')
            break;
        line[n++] = c;
    }
    line[n] = 0;
    return 0;
}

/* Copy n bytes between descriptors, checksumming them; sock says which
 * side is the socket so that side waits on readiness. Progress goes to
 * standard error once per second when it is a terminal, on the client
 * and in the server's child alike, so a long transfer is never silent. */
static int copy(int from, int to, uint64_t n, int from_sock, uint32_t *crc, const char *label)
{
    static char buf[CHUNK];
    uint64_t done = 0, total = n, start = now_ms(), shown = start;
    int tty = isatty(2);
    while (n) {
        if (from_sock && ready(from, POLLIN) < 0)
            return -1;
        ssize_t r = read(from, buf, n < sizeof buf ? (size_t)n : sizeof buf);
        if (r <= 0)
            return -1;
        *crc = crc32(*crc, buf, (size_t)r);
        if (from_sock ? (write(to, buf, (size_t)r) != r) : (send_all(to, buf, (size_t)r) < 0))
            return -1;
        n -= (uint64_t)r;
        done += (uint64_t)r;
        uint64_t t = now_ms();
        if (tty && label && t - shown >= 1000) {
            shown = t;
            fprintf(stderr, "\r%s: %llu of %llu bytes, %llu KiB/s", label,
                    (unsigned long long)done, (unsigned long long)total,
                    (unsigned long long)(done / (t - start + 1)));
        }
    }
    if (tty && label && shown != start)
        fputc('\n', stderr);
    return 0;
}

static void report(const char *verb, const char *name, uint64_t bytes, uint64_t start)
{
    uint64_t ms = now_ms() - start + 1;
    printf("xfer: %s %s, %llu bytes, %llu KiB/s\n", verb, name, (unsigned long long)bytes,
           (unsigned long long)(bytes / ms));
    fflush(stdout);
}

/* The text after the first n space separated words of line: names may
 * contain spaces, so they always come last. */
static int rest_of(const char *line, int n, char *out, size_t size)
{
    const char *p = line;
    for (int i = 0; i < n; i++) {
        p += strcspn(p, " ");
        if (*p != ' ')
            return 0;
        p++;
    }
    if (!*p || strlen(p) >= size)
        return 0;
    strcpy(out, p);
    return 1;
}

/* The name field of an "F size crc name" listing line. */
static const char *entry_name(const char *entry)
{
    static char name[256];
    return rest_of(entry, 3, name, sizeof name) ? name : "";
}

/* A relative path with no empty, "." or ".." component. */
static int safe_path(const char *p)
{
    if (!*p || *p == '/')
        return 0;
    while (*p) {
        size_t n = strcspn(p, "/");
        if (n == 0 || (n == 1 && p[0] == '.') || (n == 2 && p[0] == '.' && p[1] == '.'))
            return 0;
        p += n;
        if (*p == '/')
            p++;
    }
    return 1;
}

/* mkdir -p for the directories of a relative file path under base. */
static void make_parents(const char *base, const char *rel)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", base, rel);
    for (char *p = path + strlen(base) + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(path, 0755);
            *p = '/';
        }
    }
}

static int connect_to(void)
{
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *ai;
    int error = getaddrinfo(host, port, &hints, &ai);
    if (error) {
        fprintf(stderr, "xfer: %s: %s\n", host, gai_strerror(error));
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0 || connect(fd, ai->ai_addr, ai->ai_addrlen) < 0) {
        fprintf(stderr, "xfer: connect %s:%s: %s\n", host, port, strerror(errno));
        if (errno == ENETUNREACH || errno == EADDRNOTAVAIL)
            fprintf(stderr, "xfer: the interface has no address; run dhcpc or net config first\n");
        else if (errno == ECONNREFUSED)
            fprintf(stderr, "xfer: nothing is serving there; on the host run tools/xfer.py serve (port 9101)\n");
        freeaddrinfo(ai);
        return -1;
    }
    freeaddrinfo(ai);
    return fd;
}

/* ---- client ---- */

/* make_remote_dir asks the server to create the directory rel.  A server
 * without the MKDIR request answers with an error, which only means that
 * an empty directory is not created there. */
static int make_remote_dir(const char *rel)
{
    int fd = connect_to();
    if (fd < 0)
        return 1;
    char line[1200];
    snprintf(line, sizeof line, "MKDIR %s\n", rel);
    int status = 0;
    if (send_all(fd, line, strlen(line)) < 0 || read_line(fd, line, sizeof line) < 0)
        status = 1;
    else if (strcmp(line, "OK") != 0 && strcmp(line, "ERR bad request") != 0) {
        fprintf(stderr, "xfer: %s: %s\n", rel, line);
        status = 1;
    }
    close(fd);
    return status;
}

static int put_file(const char *path, const char *rel)
{
    int file = open(path, O_RDONLY);
    struct stat st;
    if (file < 0 || fstat(file, &st) < 0) {
        fprintf(stderr, "xfer: %s: %s\n", path, strerror(errno));
        return 1;
    }
    int fd = connect_to();
    if (fd < 0) {
        close(file);
        return 1;
    }
    char line[1200];
    uint32_t crc = 0;
    unsigned long long size = (unsigned long long)st.st_size, echoed;
    unsigned crc_back;
    uint64_t start = now_ms();
    snprintf(line, sizeof line, "PUT %llu %s\n", size, rel);
    int status = 1;
    if (send_all(fd, line, strlen(line)) == 0 && copy(file, fd, size, 0, &crc, rel) == 0 &&
        read_line(fd, line, sizeof line) == 0) {
        if (sscanf(line, "OK %llu %x", &echoed, &crc_back) == 2 && echoed == size && crc_back == crc) {
            report("sent", rel, size, start);
            status = 0;
        } else if (strncmp(line, "OK", 2) == 0) {
            fprintf(stderr, "xfer: %s: checksum mismatch, the copy is damaged\n", rel);
        } else {
            fprintf(stderr, "xfer: %s: %s\n", rel, line);
        }
    } else {
        fprintf(stderr, "xfer: %s: %s\n", rel, strerror(errno));
    }
    close(file);
    close(fd);
    return status;
}

static int put_tree(const char *path, const char *rel, int operand)
{
    struct stat st;
    /* An operand is followed; a symbolic link inside the tree is not
     * transferred, so that a link to an ancestor cannot recurse. */
    if ((operand ? stat(path, &st) : lstat(path, &st)) < 0) {
        fprintf(stderr, "xfer: %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (S_ISLNK(st.st_mode)) {
        fprintf(stderr, "xfer: %s: symbolic link skipped\n", path);
        return 0;
    }
    if (!S_ISDIR(st.st_mode))
        return put_file(path, rel);
    DIR *d = opendir(path);
    if (!d)
        return 1;
    int status = make_remote_dir(rel);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char sub[1024], subrel[1024];
        snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
        snprintf(subrel, sizeof subrel, "%s/%s", rel, e->d_name);
        status |= put_tree(sub, subrel, 0);
    }
    closedir(d);
    return status;
}

static const char *base(const char *path)
{
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/')
        n--;
    const char *end = path + n, *s = end;
    while (s > path && s[-1] != '/')
        s--;
    static char name[256];
    snprintf(name, sizeof name, "%.*s", (int)(end - s), s);
    return name;
}

static int get_file(const char *rel)
{
    int fd = connect_to();
    if (fd < 0)
        return 1;
    char line[1200];
    snprintf(line, sizeof line, "GET %s\n", rel);
    unsigned long long size;
    unsigned expect;
    if (send_all(fd, line, strlen(line)) < 0 || read_line(fd, line, sizeof line) < 0 ||
        sscanf(line, "OK %llu %x", &size, &expect) != 2) {
        fprintf(stderr, "xfer: %s: %s\n", rel, line[0] ? line : strerror(errno));
        close(fd);
        return 1;
    }
    make_parents(outdir, rel);
    char path[1200];
    snprintf(path, sizeof path, "%s/%s", outdir, rel);
    int file = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    uint32_t crc = 0;
    uint64_t start = now_ms();
    int status = 1;
    if (file < 0 || copy(fd, file, size, 1, &crc, rel) < 0) {
        fprintf(stderr, "xfer: %s: %s\n", path, strerror(errno));
    } else if (crc != expect) {
        fprintf(stderr, "xfer: %s: checksum mismatch, the copy is damaged\n", rel);
        unlink(path);
    } else {
        report("received", rel, size, start);
        status = 0;
    }
    if (file >= 0)
        close(file);
    close(fd);
    return status;
}

/* Ask the server for a listing; entries arrive as "F size crc name" or
 * "D name" lines ended by "END". Returns the lines in a malloc'd block. */
static char *list_dir(const char *rel, size_t *count, int quiet)
{
    int fd = connect_to();
    if (fd < 0)
        return NULL;
    char line[1200];
    snprintf(line, sizeof line, "LIST %s\n", rel);
    if (send_all(fd, line, strlen(line)) < 0 || read_line(fd, line, sizeof line) < 0 ||
        strncmp(line, "OK", 2) != 0) {
        if (!quiet)
            fprintf(stderr, "xfer: %s: %s\n", rel, line[0] ? line : strerror(errno));
        close(fd);
        return NULL;
    }
    size_t cap = 4096, used = 0;
    char *text = malloc(cap);
    *count = 0;
    while (text && read_line(fd, line, sizeof line) == 0 && strcmp(line, "END") != 0) {
        size_t n = strlen(line) + 1;
        if (used + n > cap) {
            cap *= 2;
            char *bigger = realloc(text, cap);
            if (!bigger) {
                free(text);
                text = NULL;
                break;
            }
            text = bigger;
        }
        memcpy(text + used, line, n);
        used += n;
        (*count)++;
    }
    close(fd);
    return text;
}

static int get_tree(const char *rel)
{
    size_t count;
    char *listing = list_dir(rel, &count, 1);
    if (!listing)
        return get_file(rel);   /* not a directory, or the error is printed */
    /* The directory is created even when it is empty. */
    char local[1200];
    make_parents(outdir, rel);
    snprintf(local, sizeof local, "%s/%s", outdir, rel);
    mkdir(local, 0755);
    int status = 0;
    char *p = listing;
    for (size_t i = 0; i < count; i++) {
        char sub[1200];
        if (p[0] == 'D') {
            snprintf(sub, sizeof sub, "%s/%s", rel, p + 2);
            status |= get_tree(sub);
        } else if (p[0] == 'F') {
            unsigned long long size;
            unsigned crc;
            if (sscanf(p, "F %llu %x", &size, &crc) == 2) {
                snprintf(sub, sizeof sub, "%s/%s", rel, entry_name(p));
                status |= get_file(sub);
            }
        }
        p += strlen(p) + 1;
    }
    free(listing);
    return status;
}

static int ls(const char *rel)
{
    size_t count;
    char *listing = list_dir(rel, &count, 0);
    if (!listing)
        return 1;
    char *p = listing;
    for (size_t i = 0; i < count; i++) {
        unsigned long long size;
        unsigned crc;
        if (p[0] == 'D')
            printf("%12s  %s/\n", "", p + 2);
        else if (sscanf(p, "F %llu %x", &size, &crc) == 2)
            printf("%12llu  %s\n", size, entry_name(p));
        p += strlen(p) + 1;
    }
    free(listing);
    return 0;
}

/* ---- server ---- */

static void reply(int c, const char *text)
{
    send_all(c, text, strlen(text));
}

static void serve_one(int c, const char *dir)
{
    char line[1200], rel[1024], path[2100], text[128];
    unsigned long long size;
    if (read_line(c, line, sizeof line) < 0)
        return;
    if (strncmp(line, "PUT ", 4) == 0 && sscanf(line + 4, "%llu", &size) == 1 && rest_of(line, 2, rel, sizeof rel)) {
        if (!safe_path(rel)) {
            reply(c, "ERR path must be relative without ..\n");
            return;
        }
        make_parents(dir, rel);
        char part[2200];
        snprintf(path, sizeof path, "%s/%s", dir, rel);
        snprintf(part, sizeof part, "%s%s", path, PART_SUFFIX);
        /* The upload goes to a temporary file, which replaces the target
         * only after the transfer completed. */
        int file = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        uint32_t crc = 0;
        uint64_t start = now_ms();
        int ok = file >= 0 && copy(c, file, size, 1, &crc, rel) == 0;
        int err = errno;
        if (file >= 0 && close(file) < 0 && ok) {
            ok = 0;
            err = errno;
        }
        if (ok && rename(part, path) < 0) {
            ok = 0;
            err = errno;
        }
        if (!ok) {
            snprintf(text, sizeof text, "ERR %s\n", strerror(err));
            unlink(part);
        } else {
            snprintf(text, sizeof text, "OK %llu %08x\n", size, crc);
            report("stored", rel, size, start);
        }
        reply(c, text);
    } else if (strncmp(line, "GET ", 4) == 0 && rest_of(line, 1, rel, sizeof rel)) {
        if (!safe_path(rel)) {
            reply(c, "ERR path must be relative without ..\n");
            return;
        }
        snprintf(path, sizeof path, "%s/%s", dir, rel);
        int file = open(path, O_RDONLY);
        struct stat st;
        if (file < 0 || fstat(file, &st) < 0 || S_ISDIR(st.st_mode)) {
            snprintf(text, sizeof text, "ERR %s\n", file >= 0 ? "is a directory" : strerror(errno));
            reply(c, text);
        } else {
            /* The checksum is computed before the header so the client can
             * verify; the file is read twice. */
            uint32_t crc = 0;
            static char buf[CHUNK];
            ssize_t r;
            while ((r = read(file, buf, sizeof buf)) > 0)
                crc = crc32(crc, buf, (size_t)r);
            lseek(file, 0, SEEK_SET);
            snprintf(text, sizeof text, "OK %llu %08x\n", (unsigned long long)st.st_size, crc);
            uint32_t again = 0;
            uint64_t start = now_ms();
            if (send_all(c, text, strlen(text)) == 0 &&
                copy(file, c, (uint64_t)st.st_size, 0, &again, rel) == 0)
                report("served", rel, (uint64_t)st.st_size, start);
        }
        if (file >= 0)
            close(file);
    } else if (strcmp(line, "LIST") == 0 || (strncmp(line, "LIST ", 5) == 0 && rest_of(line, 1, rel, sizeof rel))) {
        if (strcmp(line, "LIST") == 0 || strcmp(rel, ".") == 0)
            snprintf(path, sizeof path, "%s", dir);
        else if (safe_path(rel))
            snprintf(path, sizeof path, "%s/%s", dir, rel);
        else {
            reply(c, "ERR path must be relative without ..\n");
            return;
        }
        DIR *d = opendir(path);
        if (!d) {
            snprintf(text, sizeof text, "ERR %s\n", strerror(errno));
            reply(c, text);
            return;
        }
        reply(c, "OK\n");
        struct dirent *e;
        while ((e = readdir(d))) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                continue;
            char sub[2200];
            struct stat st;
            snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
            /* Symbolic links are not listed, so that a client that
             * descends into every directory cannot loop. */
            if (lstat(sub, &st) < 0 || S_ISLNK(st.st_mode))
                continue;
            /* The checksum field of a listing is not computed: a listing
             * must not read the files.  GET carries the checksum. */
            if (S_ISDIR(st.st_mode))
                snprintf(line, sizeof line, "D %s\n", e->d_name);
            else
                snprintf(line, sizeof line, "F %llu 00000000 %s\n", (unsigned long long)st.st_size, e->d_name);
            reply(c, line);
        }
        closedir(d);
        reply(c, "END\n");
    } else if (strncmp(line, "MKDIR ", 6) == 0 && rest_of(line, 1, rel, sizeof rel)) {
        if (!safe_path(rel)) {
            reply(c, "ERR path must be relative without ..\n");
            return;
        }
        make_parents(dir, rel);
        snprintf(path, sizeof path, "%s/%s", dir, rel);
        struct stat st;
        if (mkdir(path, 0755) < 0 && !(errno == EEXIST && stat(path, &st) == 0 && S_ISDIR(st.st_mode))) {
            snprintf(text, sizeof text, "ERR %s\n", strerror(errno));
            reply(c, text);
        } else {
            reply(c, "OK\n");
        }
    } else {
        reply(c, "ERR bad request\n");
    }
}

static int serve(const char *dir)
{
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM, .ai_flags = AI_PASSIVE}, *ai;
    int error = getaddrinfo(NULL, port, &hints, &ai);
    if (error)
        return fprintf(stderr, "xfer: %s\n", gai_strerror(error)), 1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0 || bind(fd, ai->ai_addr, ai->ai_addrlen) < 0 || listen(fd, 8) < 0)
        return fprintf(stderr, "xfer: listen on port %s: %s\n", port, strerror(errno)), 1;
    freeaddrinfo(ai);
    printf("xfer: serving %s on port %s\n", dir, port);
    fflush(stdout);
    for (;;) {
        int c = accept(fd, NULL, NULL);
        while (waitpid(-1, NULL, WNOHANG) > 0)
            ;
        if (c < 0)
            continue;
        pid_t child = fork();
        if (child == 0) {
            close(fd);
            serve_one(c, dir);
            _exit(0);
        }
        close(c);
    }
}

/* ---- main ---- */

static int usage(void)
{
    fprintf(stderr,
            "usage: xfer put [-h HOST] [-p PORT] PATH...\n"
            "       xfer get [-h HOST] [-p PORT] [-o DIR] NAME...\n"
            "       xfer ls  [-h HOST] [-p PORT] [DIR]\n"
            "       xfer serve [-p PORT] [DIR]\n"
            "HOST defaults to %s (the host under QEMU), PORT to %s, and to %s for serve.\n", DEFAULT_HOST,
            DEFAULT_PORT, DEFAULT_SERVE_PORT);
    return 2;
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return usage();
    const char *cmd = argv[1];
    int i = 2;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (strcmp(argv[i], "-h") == 0 && i + 1 < argc) host = argv[++i];
        else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) port = argv[++i];
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) outdir = argv[++i];
        else return usage();
    }
    int status = 0;
    if (!port)
        port = strcmp(cmd, "serve") == 0 ? DEFAULT_SERVE_PORT : DEFAULT_PORT;
    if (strcmp(cmd, "serve") == 0)
        return serve(i < argc ? argv[i] : ".");
    if (strcmp(cmd, "put") == 0) {
        if (i >= argc)
            return usage();
        for (; i < argc; i++)
            status |= put_tree(argv[i], base(argv[i]), 1);
    } else if (strcmp(cmd, "get") == 0) {
        if (i >= argc)
            return usage();
        for (; i < argc; i++)
            status |= get_tree(argv[i]);
    } else if (strcmp(cmd, "ls") == 0) {
        status = ls(i < argc ? argv[i] : ".");
    } else {
        return usage();
    }
    if (status)
        failures++;
    return status ? 1 : 0;
}
