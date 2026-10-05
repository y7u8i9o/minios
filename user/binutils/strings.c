/* strings: print the runs of printable characters in files, with the
 * options of GNU strings.  The characters are 7 bit or 8 bit bytes, or
 * 16 bit or 32 bit units of either byte order.  Without a file the
 * program reads the standard input.
 *
 *     strings [-adfowT] [-n length] [-t radix] [-e encoding] [-s separator] [file...]
 */
#include "lib/binutils.h"
#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int opt_data, opt_names, opt_whitespace, opt_radix;
static int encoding = 's', width = 1;
static size_t min_length = 4;
static const char *separator;
static unsigned char *line;

/* Report whether c is a character that a string may contain. */
static int graphic(long c)
{
    if (c < 0 || c > 255)
        return 0;
    if (c == '\t' || (c >= 0x20 && c < 0x7f))
        return 1;
    if (encoding == 'S' && c > 127)
        return 1;
    return opt_whitespace && (c == ' ' || (c >= '\t' && c <= '\r'));
}

/* Read the character that starts at the byte pos of data. */
static long unit(const uint8_t *d)
{
    switch (encoding) {
    case 'b':
        return (long)((d[0] << 8) | d[1]);
    case 'l':
        return (long)(d[0] | (d[1] << 8));
    case 'B':
        return ((long)d[0] << 24) | ((long)d[1] << 16) | (d[2] << 8) | d[3];
    case 'L':
        return d[0] | (d[1] << 8) | ((long)d[2] << 16) | ((long)d[3] << 24);
    default:
        return d[0];
    }
}

static void print_prefix(const char *name, uint64_t start)
{
    if (opt_names)
        printf("%s: ", name);
    if (opt_radix == 'o')
        printf("%7llo ", (unsigned long long)start);
    else if (opt_radix == 'd')
        printf("%7llu ", (unsigned long long)start);
    else if (opt_radix == 'x')
        printf("%7llx ", (unsigned long long)start);
}

/* Print the strings in len bytes of data.  The first byte has the
 * address base.  A unit that is no string character consumes one byte, so
 * the next unit starts one byte later, as in GNU strings. */
static void scan(const char *name, const uint8_t *data, size_t len, uint64_t base)
{
    size_t pos = 0;
    for (;;) {
        uint64_t start = base + pos;
        size_t i;
        for (i = 0; i < min_length; i++) {
            if (pos + (size_t)width > len)
                return;
            long c = unit(data + pos);
            pos += (size_t)width;
            if (!graphic(c)) {
                pos -= (size_t)width - 1;
                break;
            }
            line[i] = (unsigned char)c;
        }
        if (i < min_length)
            continue;
        print_prefix(name, start);
        fwrite(line, 1, min_length, stdout);
        while (pos + (size_t)width <= len) {
            long c = unit(data + pos);
            pos += (size_t)width;
            if (!graphic(c)) {
                pos -= (size_t)width - 1;
                break;
            }
            putchar((int)c);
        }
        if (separator)
            fputs(separator, stdout);
        else
            putchar('\n');
    }
}

/* Scan only the sections of an ELF file that are loaded from the file.
 * The result is 0 when the file has no such section. */
static int scan_sections(const char *name, const uint8_t *data, size_t len)
{
    struct elffile f;
    if (elffile_open(&f, data, len) < 0)
        return 0;
    int found = 0;
    for (unsigned i = 1; i < f.nsections; i++) {
        const Elf64_Shdr *s = &f.sh[i];
        if (!(s->sh_flags & SHF_ALLOC) || s->sh_type == SHT_NOBITS)
            continue;
        found = 1;
        if (s->sh_offset > len)
            continue;
        size_t size = s->sh_size;
        if (size > len - s->sh_offset)
            size = len - s->sh_offset;
        scan(name, data + s->sh_offset, size, s->sh_offset);
    }
    return found;
}

static int read_all(int fd, uint8_t **data, size_t *size)
{
    size_t cap = 65536, n = 0;
    uint8_t *buf = bu_alloc(cap);
    for (;;) {
        if (n == cap) {
            cap *= 2;
            buf = bu_realloc(buf, cap);
        }
        ssize_t r = read(fd, buf + n, cap - n);
        if (r < 0) {
            free(buf);
            return -errno;
        }
        if (r == 0)
            break;
        n += (size_t)r;
    }
    *data = buf;
    *size = n;
    return 0;
}

static int do_file(const char *path)
{
    uint8_t *data;
    size_t size;
    int r = bu_read_file(path, &data, &size);
    if (r == -ENOENT) {
        bu_error(NULL, "'%s': No such file", path);
        return 1;
    }
    if (r == -EISDIR) {
        bu_error(NULL, "Warning: '%s' is a directory", path);
        return 1;
    }
    if (r < 0) {
        bu_error(path, "%s", strerror(-r));
        return 1;
    }
    if (!opt_data || !scan_sections(path, data, size))
        scan(path, data, size, 0);
    free(data);
    return 0;
}

static int do_stdin(void)
{
    uint8_t *data = NULL;
    size_t size = 0;
    int r = read_all(0, &data, &size);
    if (r < 0) {
        bu_error("{standard input}", "%s", strerror(-r));
        return 1;
    }
    scan("{standard input}", data, size, 0);
    free(data);
    return 0;
}

static const struct option longopts[] = {
    { "all", no_argument, NULL, 'a' },
    { "data", no_argument, NULL, 'd' },
    { "print-file-name", no_argument, NULL, 'f' },
    { "bytes", required_argument, NULL, 'n' },
    { "radix", required_argument, NULL, 't' },
    { "include-all-whitespace", no_argument, NULL, 'w' },
    { "encoding", required_argument, NULL, 'e' },
    { "output-separator", required_argument, NULL, 's' },
    { "target", required_argument, NULL, 'T' },
    { "unicode", required_argument, NULL, 'U' },
    { "help", no_argument, NULL, 'h' },
    { "version", no_argument, NULL, 'V' },
    { NULL, 0, NULL, 0 },
};

static void usage(int status)
{
    fprintf(status ? stderr : stdout,
            "Usage: strings [-adfowT] [-n length] [-t radix] [-e encoding] [-s separator] [file...]\n");
    exit(status);
}

static void set_length(const char *s)
{
    char *end;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 0);
    if (*s == '\0' || *end != '\0' || errno)
        bu_fatal(NULL, "invalid integer argument %s", s);
    if (v < 1)
        bu_fatal(NULL, "minimum string length is too small: %llu", v);
    min_length = (size_t)v;
}

static void set_radix(const char *s)
{
    if (s[0] && !s[1] && (s[0] == 'o' || s[0] == 'd' || s[0] == 'x'))
        opt_radix = s[0];
    else
        usage(1);
}

static void set_encoding(const char *s)
{
    if (!s[0] || s[1] || !strchr("sSblBL", s[0]))
        usage(1);
    encoding = s[0];
    width = (s[0] == 'b' || s[0] == 'l') ? 2 : (s[0] == 'B' || s[0] == 'L') ? 4 : 1;
}

static void apply(int code, const char *arg)
{
    switch (code) {
    case 'a': opt_data = 0; break;
    case 'd': opt_data = 1; break;
    case 'f': opt_names = 1; break;
    case 'n': set_length(arg); break;
    case 't': set_radix(arg); break;
    case 'o': opt_radix = 'o'; break;
    case 'w': opt_whitespace = 1; break;
    case 'e': set_encoding(arg); break;
    case 's': separator = arg; break;
    case 'T': case 'U': break;
    case 'v': case 'V': printf("strings (minios binutils)\n"); exit(0);
    case 'h': usage(0); break;
    default: usage(1); break;
    }
}

/* The argument -NUMBER is the short form of -n NUMBER.  rewrite_numbers
 * replaces each such argument before the first "--" with "-nNUMBER".  A
 * single "-" is no file name for strings and gives the usage message. */
static void rewrite_numbers(int argc, char **argv)
{
    for (int i = 1; i < argc && strcmp(argv[i], "--") != 0; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-") == 0)
            usage(1);
        if (a[0] != '-' || a[1] < '0' || a[1] > '9' || strspn(a + 1, "0123456789") != strlen(a + 1))
            continue;
        size_t len = strlen(a) + 2;
        char *n = bu_alloc(len);
        snprintf(n, len, "-n%s", a + 1);
        argv[i] = n;
    }
}

int main(int argc, char **argv)
{
    bu_program = "strings";
    rewrite_numbers(argc, argv);
    int c;
    while ((c = getopt_long(argc, argv, "adfn:t:owe:s:T:U:hvV", longopts, NULL)) != -1)
        apply(c, optarg);
    char **files = argv + optind;
    int nfiles = argc - optind;
    line = bu_alloc(min_length + 1);
    int status = 0;
    if (nfiles == 0) {
        status = do_stdin();
    } else {
        for (int i = 0; i < nfiles; i++)
            if (do_file(files[i]))
                status = 1;
    }
    fflush(stdout);
    return status;
}
