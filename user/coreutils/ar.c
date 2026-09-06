/* ar: maintain archives of files in the System V format that the GNU
 * binutils write: a member header of 60 printable bytes, short names
 * terminated with a slash, long names in the "//" name table, member data
 * padded to an even length. A symbol table ("/") in an existing archive is
 * dropped when the archive is rewritten, because no linker on minios reads
 * one. BSD "#1/n" names are understood when reading.
 *
 *     ar [-]{dpqrtx}[cousv] archive [member...]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <ar.h>
#include <sys/stat.h>
#include <libgen.h>

struct member {
    char *name;
    long date;
    int uid, gid;
    unsigned mode;
    size_t size;
    char *data;          /* owned when from_file is set, else into the image */
    int from_file;
    struct member *next;
};

static const char *archive_path;
static char *image;
static size_t image_size;
static struct member *members;
static int verbose, create_quiet, update_only, preserve_dates;

static void die(const char *what, const char *detail)
{
    if (detail != NULL)
        fprintf(stderr, "ar: %s: %s\n", detail, what);
    else
        fprintf(stderr, "ar: %s\n", what);
    exit(1);
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (p == NULL)
        die("out of memory", NULL);
    return p;
}

static char *xstrndup(const char *s, size_t n)
{
    char *copy = xmalloc(n + 1);
    memcpy(copy, s, n);
    copy[n] = '\0';
    return copy;
}

/* A number field of the header: decimal digits followed by spaces. */
static long field_number(const char *field, size_t len, int base)
{
    char buf[24];
    if (len >= sizeof buf)
        len = sizeof buf - 1;
    memcpy(buf, field, len);
    buf[len] = '\0';
    return strtol(buf, NULL, base);
}

static void append_member(struct member *m)
{
    m->next = NULL;
    struct member **tail = &members;
    while (*tail != NULL)
        tail = &(*tail)->next;
    *tail = m;
}

/* Read the archive into memory and build the member list. A missing
 * archive leaves the list empty. */
static void read_archive(void)
{
    int fd = open(archive_path, O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT)
            return;
        die(strerror(errno), archive_path);
    }
    struct stat st;
    if (fstat(fd, &st) < 0)
        die(strerror(errno), archive_path);
    image_size = (size_t)st.st_size;
    image = xmalloc(image_size);
    size_t got = 0;
    while (got < image_size) {
        ssize_t n = read(fd, image + got, image_size - got);
        if (n <= 0)
            die("short read", archive_path);
        got += (size_t)n;
    }
    close(fd);
    if (image_size < SARMAG || memcmp(image, ARMAG, SARMAG) != 0)
        die("not an archive", archive_path);

    const char *names = NULL;
    size_t names_len = 0;
    size_t off = SARMAG;
    while (off + sizeof(struct ar_hdr) <= image_size) {
        const struct ar_hdr *h = (const struct ar_hdr *)(image + off);
        if (memcmp(h->ar_fmag, ARFMAG, 2) != 0)
            die("bad member header", archive_path);
        size_t size = (size_t)field_number(h->ar_size, sizeof h->ar_size, 10);
        char *data = image + off + sizeof(struct ar_hdr);
        if (data + size > image + image_size)
            die("member extends past the end", archive_path);
        size_t next = off + sizeof(struct ar_hdr) + size + (size & 1);
        char *name;
        if (h->ar_name[0] == '/' && h->ar_name[1] == '/') {
            names = data;
            names_len = size;
            off = next;
            continue;
        }
        if (h->ar_name[0] == '/' && (h->ar_name[1] == ' ' || h->ar_name[1] == 'S')) {
            off = next;                          /* symbol table or /SYM64/ */
            continue;
        }
        if (h->ar_name[0] == '/') {
            size_t pos = (size_t)field_number(h->ar_name + 1, sizeof h->ar_name - 1, 10);
            if (names == NULL || pos >= names_len)
                die("bad long name reference", archive_path);
            const char *end = memchr(names + pos, '\n', names_len - pos);
            size_t len = end != NULL ? (size_t)(end - (names + pos)) : names_len - pos;
            if (len > 0 && names[pos + len - 1] == '/')
                len--;
            name = xstrndup(names + pos, len);
        } else if (memcmp(h->ar_name, "#1/", 3) == 0) {
            size_t len = (size_t)field_number(h->ar_name + 3, sizeof h->ar_name - 3, 10);
            if (len > size)
                die("bad BSD name length", archive_path);
            name = xstrndup(data, strnlen(data, len));
            data += len;
            size -= len;
        } else {
            size_t len = 0;
            while (len < sizeof h->ar_name && h->ar_name[len] != '/' && h->ar_name[len] != ' ')
                len++;
            if (len == sizeof h->ar_name) {
                while (len > 0 && h->ar_name[len - 1] == ' ')
                    len--;
            }
            name = xstrndup(h->ar_name, len);
        }
        struct member *m = xmalloc(sizeof *m);
        m->name = name;
        m->date = field_number(h->ar_date, sizeof h->ar_date, 10);
        m->uid = (int)field_number(h->ar_uid, sizeof h->ar_uid, 10);
        m->gid = (int)field_number(h->ar_gid, sizeof h->ar_gid, 10);
        m->mode = (unsigned)field_number(h->ar_mode, sizeof h->ar_mode, 8);
        m->size = size;
        m->data = data;
        m->from_file = 0;
        append_member(m);
        off = next;
    }
}

static void write_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w <= 0)
            die(strerror(errno), archive_path);
        p += w;
        n -= (size_t)w;
    }
}

static void put_field(char *field, size_t len, const char *text)
{
    size_t n = strlen(text);
    if (n > len)
        n = len;
    memcpy(field, text, n);
    memset(field + n, ' ', len - n);
}

static void put_number(char *field, size_t len, long value, int base)
{
    char buf[24];
    snprintf(buf, sizeof buf, base == 8 ? "%lo" : "%ld", value);
    put_field(field, len, buf);
}

/* Write the members to a temporary file beside the archive and rename it
 * over the archive. Long names go into a "//" table written first. */
static void write_archive(void)
{
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s.tmp", archive_path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        die(strerror(errno), tmp);
    write_all(fd, ARMAG, SARMAG);

    size_t table_len = 0;
    for (struct member *m = members; m != NULL; m = m->next)
        if (strlen(m->name) > 15)
            table_len += strlen(m->name) + 2;
    char *table = NULL;
    if (table_len > 0) {
        table = xmalloc(table_len + 1);
        size_t pos = 0;
        for (struct member *m = members; m != NULL; m = m->next) {
            if (strlen(m->name) > 15) {
                pos += (size_t)sprintf(table + pos, "%s/\n", m->name);
            }
        }
        struct ar_hdr h;
        put_field(h.ar_name, sizeof h.ar_name, "//");
        put_field(h.ar_date, sizeof h.ar_date, "");
        put_field(h.ar_uid, sizeof h.ar_uid, "");
        put_field(h.ar_gid, sizeof h.ar_gid, "");
        put_field(h.ar_mode, sizeof h.ar_mode, "");
        put_number(h.ar_size, sizeof h.ar_size, (long)table_len, 10);
        memcpy(h.ar_fmag, ARFMAG, 2);
        write_all(fd, &h, sizeof h);
        write_all(fd, table, table_len);
        if (table_len & 1)
            write_all(fd, "\n", 1);
    }

    size_t table_pos = 0;
    for (struct member *m = members; m != NULL; m = m->next) {
        struct ar_hdr h;
        char name[24];
        if (strlen(m->name) > 15) {
            snprintf(name, sizeof name, "/%zu", table_pos);
            table_pos += strlen(m->name) + 2;
        } else {
            snprintf(name, sizeof name, "%s/", m->name);
        }
        put_field(h.ar_name, sizeof h.ar_name, name);
        put_number(h.ar_date, sizeof h.ar_date, m->date, 10);
        put_number(h.ar_uid, sizeof h.ar_uid, m->uid, 10);
        put_number(h.ar_gid, sizeof h.ar_gid, m->gid, 10);
        put_number(h.ar_mode, sizeof h.ar_mode, (long)m->mode, 8);
        put_number(h.ar_size, sizeof h.ar_size, (long)m->size, 10);
        memcpy(h.ar_fmag, ARFMAG, 2);
        write_all(fd, &h, sizeof h);
        write_all(fd, m->data, m->size);
        if (m->size & 1)
            write_all(fd, "\n", 1);
    }
    close(fd);
    free(table);
    if (rename(tmp, archive_path) < 0)
        die(strerror(errno), archive_path);
}

static struct member *find_member(const char *name)
{
    for (struct member *m = members; m != NULL; m = m->next)
        if (strcmp(m->name, name) == 0)
            return m;
    return NULL;
}

static char *member_name_of(const char *path)
{
    char *copy = xstrndup(path, strlen(path));
    char *base = basename(copy);
    char *name = xstrndup(base, strlen(base));
    free(copy);
    return name;
}

/* Read a file into a fresh member. */
static struct member *member_from_file(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        die(strerror(errno), path);
    struct stat st;
    if (fstat(fd, &st) < 0)
        die(strerror(errno), path);
    struct member *m = xmalloc(sizeof *m);
    m->name = member_name_of(path);
    m->date = (long)st.st_mtime;
    m->uid = (int)st.st_uid;
    m->gid = (int)st.st_gid;
    m->mode = st.st_mode & 07777;
    m->size = (size_t)st.st_size;
    m->data = xmalloc(m->size);
    m->from_file = 1;
    size_t got = 0;
    while (got < m->size) {
        ssize_t n = read(fd, m->data + got, m->size - got);
        if (n <= 0)
            die("short read", path);
        got += (size_t)n;
    }
    close(fd);
    return m;
}

/* True when the member is named by the arguments, or when there are none. */
static int selected(const char *name, int argc, char **argv)
{
    if (argc == 0)
        return 1;
    for (int i = 0; i < argc; i++) {
        char *want = member_name_of(argv[i]);
        int match = strcmp(want, name) == 0;
        free(want);
        if (match)
            return 1;
    }
    return 0;
}

static void op_table(int argc, char **argv)
{
    for (struct member *m = members; m != NULL; m = m->next) {
        if (!selected(m->name, argc, argv))
            continue;
        if (verbose) {
            char when[32];
            time_t t = (time_t)m->date;
            struct tm tm;
            localtime_r(&t, &tm);
            strftime(when, sizeof when, "%b %e %H:%M %Y", &tm);
            char perm[10];
            for (int i = 0; i < 9; i++)
                perm[i] = (m->mode & (0400 >> i)) ? "rwx"[i % 3] : '-';
            perm[9] = '\0';
            printf("%s %d/%d %6zu %s %s\n", perm, m->uid, m->gid, m->size, when, m->name);
        } else {
            printf("%s\n", m->name);
        }
    }
}

static void op_print(int argc, char **argv)
{
    for (struct member *m = members; m != NULL; m = m->next) {
        if (!selected(m->name, argc, argv))
            continue;
        if (verbose)
            printf("\n<%s>\n\n", m->name);
        fflush(stdout);
        write_all(1, m->data, m->size);
    }
}

static void op_extract(int argc, char **argv)
{
    for (struct member *m = members; m != NULL; m = m->next) {
        if (!selected(m->name, argc, argv))
            continue;
        if (verbose)
            printf("x - %s\n", m->name);
        int fd = open(m->name, O_WRONLY | O_CREAT | O_TRUNC, m->mode ? m->mode : 0644);
        if (fd < 0)
            die(strerror(errno), m->name);
        write_all(fd, m->data, m->size);
        close(fd);
        if (preserve_dates) {
            struct timespec times[2] = { { 0, UTIME_OMIT }, { m->date, 0 } };
            utimensat(AT_FDCWD, m->name, times, 0);
        }
    }
}

static void op_delete(int argc, char **argv)
{
    struct member **link = &members;
    while (*link != NULL) {
        struct member *m = *link;
        if (argc > 0 && selected(m->name, argc, argv)) {
            if (verbose)
                printf("d - %s\n", m->name);
            *link = m->next;
        } else {
            link = &m->next;
        }
    }
    write_archive();
}

static void op_replace(int argc, char **argv, int quick)
{
    for (int i = 0; i < argc; i++) {
        struct member *fresh = member_from_file(argv[i]);
        struct member *old = quick ? NULL : find_member(fresh->name);
        if (old != NULL) {
            if (update_only && fresh->date <= old->date)
                continue;
            if (verbose)
                printf("r - %s\n", fresh->name);
            old->date = fresh->date;
            old->uid = fresh->uid;
            old->gid = fresh->gid;
            old->mode = fresh->mode;
            old->size = fresh->size;
            old->data = fresh->data;
            old->from_file = 1;
        } else {
            if (verbose)
                printf("a - %s\n", fresh->name);
            append_member(fresh);
        }
    }
    write_archive();
}

static void usage(void)
{
    fprintf(stderr, "usage: ar [-]{dpqrtx}[cousv] archive [member...]\n");
    exit(2);
}

int main(int argc, char **argv)
{
    if (argc < 3)
        usage();
    const char *opts = argv[1];
    if (opts[0] == '-')
        opts++;
    char op = 0;
    for (const char *p = opts; *p != '\0'; p++) {
        switch (*p) {
        case 'd': case 'p': case 'q': case 'r': case 't': case 'x':
            if (op != 0)
                usage();
            op = *p;
            break;
        case 'c': create_quiet = 1; break;
        case 'o': preserve_dates = 1; break;
        case 'u': update_only = 1; break;
        case 's': break;
        case 'v': verbose = 1; break;
        default: usage();
        }
    }
    if (op == 0)
        usage();
    archive_path = argv[2];
    read_archive();
    if (image == NULL) {
        if (op != 'r' && op != 'q')
            die("no such archive", archive_path);
        if (!create_quiet)
            fprintf(stderr, "ar: creating %s\n", archive_path);
    }
    int nargs = argc - 3;
    char **args = argv + 3;
    switch (op) {
    case 't': op_table(nargs, args); break;
    case 'p': op_print(nargs, args); break;
    case 'x': op_extract(nargs, args); break;
    case 'd': op_delete(nargs, args); break;
    case 'r': op_replace(nargs, args, 0); break;
    case 'q': op_replace(nargs, args, 1); break;
    }
    return 0;
}
