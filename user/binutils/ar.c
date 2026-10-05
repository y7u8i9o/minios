/* ar: maintain archives of files in the System V format that the GNU
 * binutils write (lib/archive.c).  Every rewrite of an archive writes the
 * symbol index "/" of the defined global symbols of its ELF members, as
 * GNU ar does, unless the modifier S is given.  The operation s alone
 * writes the index of an archive without other changes, as ranlib does.
 *
 *     ar [-]{dpqrstx}[cousSv] archive [member...]
 */
#include "lib/binutils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <libgen.h>

static const char *archive_path;
static uint8_t *image;
static size_t image_size;
static struct archive archive;
static int verbose, create_quiet, update_only, preserve_dates, no_index;

/* Read the archive into memory and build the member list. A missing
 * archive leaves the list empty. */
static void read_archive(void)
{
    int r = bu_read_file(archive_path, &image, &image_size);
    if (r == -ENOENT) {
        image = NULL;
        return;
    }
    if (r < 0)
        bu_fatal(archive_path, "%s", strerror(-r));
    char err[200];
    if (archive_parse(&archive, image, image_size, err, sizeof err) < 0)
        bu_fatal(archive_path, "%s", err);
}

static void write_archive(void)
{
    int r = archive_write(&archive, archive_path, !no_index);
    if (r < 0)
        bu_fatal(archive_path, "%s", strerror(-r));
}

static void write_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w <= 0)
            bu_fatal(archive_path, "%s", strerror(errno));
        p += w;
        n -= (size_t)w;
    }
}

static struct ar_member *find_member(const char *name)
{
    for (struct ar_member *m = archive.members; m != NULL; m = m->next)
        if (strcmp(m->name, name) == 0)
            return m;
    return NULL;
}

static char *member_name_of(const char *path)
{
    char *copy = bu_strdup(path);
    char *base = basename(copy);
    char *name = bu_strdup(base);
    free(copy);
    return name;
}

/* Read a file into a fresh member. */
static struct ar_member *member_from_file(const char *path)
{
    uint8_t *data;
    size_t size;
    int r = bu_read_file(path, &data, &size);
    if (r < 0)
        bu_fatal(path, "%s", strerror(-r));
    struct stat st;
    if (stat(path, &st) < 0)
        bu_fatal(path, "%s", strerror(errno));
    struct ar_member *m = bu_alloc(sizeof *m);
    memset(m, 0, sizeof *m);
    m->name = member_name_of(path);
    m->date = (long)st.st_mtime;
    m->uid = (int)st.st_uid;
    m->gid = (int)st.st_gid;
    m->mode = st.st_mode & 07777;
    m->size = size;
    m->data = data;
    m->owned = data;
    return m;
}

/* True when an argument gives the member name, or when there are no arguments. */
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
    for (struct ar_member *m = archive.members; m != NULL; m = m->next) {
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
    for (struct ar_member *m = archive.members; m != NULL; m = m->next) {
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
    for (struct ar_member *m = archive.members; m != NULL; m = m->next) {
        if (!selected(m->name, argc, argv))
            continue;
        if (verbose)
            printf("x - %s\n", m->name);
        int fd = open(m->name, O_WRONLY | O_CREAT | O_TRUNC, m->mode ? m->mode : 0644);
        if (fd < 0)
            bu_fatal(m->name, "%s", strerror(errno));
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
    struct ar_member **link = &archive.members;
    while (*link != NULL) {
        struct ar_member *m = *link;
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
        struct ar_member *fresh = member_from_file(argv[i]);
        struct ar_member *old = quick ? NULL : find_member(fresh->name);
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
            free(old->owned);
            old->owned = fresh->owned;
            free(fresh->name);
            free(fresh);
        } else {
            if (verbose)
                printf("a - %s\n", fresh->name);
            archive_append(&archive, fresh);
        }
    }
    write_archive();
}

static void usage(void)
{
    fprintf(stderr, "usage: ar [-]{dpqrstx}[cousSv] archive [member...]\n");
    exit(2);
}

int main(int argc, char **argv)
{
    bu_program = "ar";
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
        case 'S': no_index = 1; break;
        case 's':
            /* s alone is the operation, s with an operation a modifier. */
            if (op == 0 && p[1] == '\0' && p == opts)
                op = 's';
            break;
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
            bu_fatal(archive_path, "No such file or directory");
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
    case 's': write_archive(); break;
    }
    return 0;
}
