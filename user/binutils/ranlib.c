/* ranlib: write the symbol index of archives, as GNU ranlib does.  The
 * index "/" lists the defined global symbols of the ELF members in the
 * order of the members and of their symbol tables (lib/archive.c).
 *
 *     ranlib [-DUt] archive...
 *
 * The option -D writes the date 0 into the index member.  The option -U
 * writes the current time and is the default.  The option -t rewrites
 * nothing.  -t sets the date of an existing index to the modification time
 * of the archive plus 60 seconds when the index is older than the archive.
 * -t reports an archive without an index.  The dates and owners of the
 * other members are unchanged. */
#include "lib/binutils.h"
#include <ar.h>
#include <errno.h>
#include <getopt.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

/* The date of the index member: 0, the current time or the time of the
 * archive. */
enum { DATE_NOW, DATE_ZERO, DATE_ARCHIVE };

/* The date field in the header of the first member. */
#define INDEX_DATE_OFFSET (SARMAG + offsetof(struct ar_hdr, ar_date))
#define INDEX_DATE_LEN (sizeof((struct ar_hdr *)0)->ar_date)
/* GNU ranlib writes the time of the archive plus this offset. */
#define ARMAP_TIME_OFFSET 60

static void usage(FILE *out)
{
    fprintf(out, "Usage: ranlib [options] archive\n"
                 " Generate an index to speed access to archives\n"
                 " The options are:\n"
                 "  -D                           Use zero for symbol map timestamp\n"
                 "  -U                           Use actual symbol map timestamp (default)\n"
                 "  -t                           Update the archive's symbol map timestamp\n"
                 "  -h --help                    Print this help message\n"
                 "  -v --version                 Print version information\n");
}

/* set_index_date writes the date into the header of the index member of
 * the archive at path, when the archive starts with an index.  ranlib -t
 * changes only this field, as GNU ranlib does. */
static int set_index_date(const char *path, long date)
{
    uint8_t *data;
    size_t size;
    int r = bu_read_file(path, &data, &size);
    if (r < 0)
        return r;
    if (size >= INDEX_DATE_OFFSET + INDEX_DATE_LEN && memcmp(data + 8, "/ ", 2) == 0) {
        char text[INDEX_DATE_LEN + 1];
        snprintf(text, sizeof text, "%-*ld", (int)INDEX_DATE_LEN, date);
        memcpy(data + INDEX_DATE_OFFSET, text, INDEX_DATE_LEN);
        struct stat st;
        unsigned mode = stat(path, &st) == 0 ? (unsigned)(st.st_mode & 07777) : 0644;
        r = bu_write_file(path, data, size, mode);
    }
    free(data);
    return r;
}

/* ranlib_file rewrites one archive with its symbol index.  The result is
 * 0 or 1 after a message. */
static int ranlib_file(const char *path, int date_mode)
{
    uint8_t *image;
    size_t size;
    int r = bu_read_file(path, &image, &size);
    if (r == -ENOENT) {
        bu_error(NULL, "'%s': No such file", path);
        return 1;
    }
    if (r < 0) {
        bu_error(path, "%s", strerror(-r));
        return 1;
    }
    struct archive a;
    char err[200];
    if (archive_parse(&a, image, size, err, sizeof err) < 0) {
        bu_error(path, "file format not recognized");
        free(image);
        return 1;
    }
    struct stat st;
    long mtime = stat(path, &st) == 0 ? (long)st.st_mtime : (long)time(NULL);
    if (date_mode == DATE_ARCHIVE) {
        /* The date of the index member is a decimal field of 12 bytes. */
        long date = a.index && size >= INDEX_DATE_OFFSET + INDEX_DATE_LEN ? atol((const char *)image + INDEX_DATE_OFFSET) : -1;
        archive_free(&a);
        free(image);
        if (date < 0) {
            bu_error(path, "no archive map to update");
            return 1;
        }
        if (date < mtime && (r = set_index_date(path, mtime + ARMAP_TIME_OFFSET)) < 0) {
            bu_error(path, "%s", strerror(-r));
            return 1;
        }
        return 0;
    }
    a.index_date = date_mode == DATE_ZERO ? 0 : (long)time(NULL);
    r = archive_write(&a, path, 1);
    archive_free(&a);
    free(image);
    if (r < 0) {
        bu_error(path, "%s", strerror(-r));
        return 1;
    }
    return 0;
}

static const struct option longopts[] = {
    { "help", no_argument, NULL, 'h' },
    { "version", no_argument, NULL, 'V' },
    { NULL, 0, NULL, 0 },
};

int main(int argc, char **argv)
{
    bu_program = "ranlib";
    int date_mode = DATE_NOW, status = 0, c;
    while ((c = getopt_long(argc, argv, "DUthvV", longopts, NULL)) != -1) {
        switch (c) {
        case 'D': date_mode = DATE_ZERO; break;
        case 'U': date_mode = DATE_NOW; break;
        case 't': date_mode = DATE_ARCHIVE; break;
        case 'h': usage(stdout); return 0;
        case 'v': case 'V': printf("GNU ranlib (minios binutils)\n"); return 0;
        default: usage(stderr); return 1;
        }
    }
    if (optind == argc) {
        usage(stderr);
        return 1;
    }
    for (int i = optind; i < argc; i++)
        status |= ranlib_file(argv[i], date_mode);
    return status;
}
