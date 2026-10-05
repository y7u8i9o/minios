/* strip: remove symbols and sections from ELF files and from the ELF
 * members of archives (elfrewrite.c).  Without an option for symbols the
 * program removes all symbols, as -s does.
 *
 *     strip [options] file...
 */
#include "elfrewrite.h"
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *to)
{
    fprintf(to,
            "Usage: %s <option(s)> in-file(s)\n"
            " Removes symbols and sections from files\n"
            " The options are:\n"
            "  -s --strip-all                   Remove all symbol and relocation information\n"
            "  -g -S -d --strip-debug           Remove debugging symbols only\n"
            "     --strip-unneeded              Remove all symbols not needed by relocations\n"
            "  -R --remove-section=<name>       Also remove section <name> from the file\n"
            "  -N --strip-symbol=<name>         Do not copy symbol <name>\n"
            "  -K --keep-symbol=<name>          Do not strip symbol <name>\n"
            "  -o <file>                        Place stripped output into <file>\n"
            "  -p --preserve-dates              Copy modified/access timestamps to the output\n"
            "  -h --help                        Display this output\n"
            "  -V --version                     Display this program's version number\n",
            bu_program);
}

static const struct option longopts[] = {
    { "strip-all", no_argument, NULL, 's' },
    { "strip-debug", no_argument, NULL, 'g' },
    { "strip-unneeded", no_argument, NULL, ELFREWRITE_OPT_STRIP_UNNEEDED },
    { "preserve-dates", no_argument, NULL, 'p' },
    { "remove-section", required_argument, NULL, 'R' },
    { "strip-symbol", required_argument, NULL, 'N' },
    { "keep-symbol", required_argument, NULL, 'K' },
    { "input-target", required_argument, NULL, 'I' },
    { "output-target", required_argument, NULL, 'O' },
    { "help", no_argument, NULL, 'h' },
    { "version", no_argument, NULL, 'V' },
    { NULL, 0, NULL, 0 },
};

/* The letters of strip.  -S is --strip-debug here, unlike in objcopy. */
static int option_code(int c)
{
    switch (c) {
    case 's': return ELFREWRITE_OPT_STRIP_ALL;
    case 'g': case 'S': case 'd': return ELFREWRITE_OPT_STRIP_DEBUG;
    case 'p': return ELFREWRITE_OPT_PRESERVE_DATES;
    case 'R': return ELFREWRITE_OPT_REMOVE_SECTION;
    case 'N': return ELFREWRITE_OPT_STRIP_SYMBOL;
    case 'K': return ELFREWRITE_OPT_RETAIN_SYMBOL;
    case 'o': return ELFREWRITE_OPT_OUTPUT;
    case 'I': return ELFREWRITE_OPT_INPUT_FORMAT;
    case 'O': return ELFREWRITE_OPT_OUTPUT_FORMAT;
    default: return c;
    }
}

int main(int argc, char **argv)
{
    bu_program = "strip";
    struct elfrewrite_options o;
    memset(&o, 0, sizeof o);
    int c;
    while ((c = getopt_long(argc, argv, "sSgdpR:N:K:o:I:O:hV", longopts, NULL)) != -1) {
        if (c == 'h') {
            usage(stdout);
            return 0;
        }
        if (c == 'V') {
            printf("strip (minios binutils)\n");
            return 0;
        }
        if (c == '?' || elfrewrite_apply(&o, option_code(c), optarg) < 0) {
            usage(stderr);
            return 1;
        }
    }
    char **files = argv + optind;
    int nfiles = argc - optind;
    if (nfiles == 0) {
        usage(stderr);
        return 1;
    }
    if (o.output && nfiles > 1)
        bu_fatal(NULL, "multiple input files with -o are not supported");
    if (o.strip == ELFREWRITE_STRIP_NONE && o.strip_symbols.count == 0)
        o.strip = ELFREWRITE_STRIP_ALL;
    int status = 0;
    for (int i = 0; i < nfiles; i++)
        if (elfrewrite_path(files[i], o.output, &o, 0))
            status = 1;
    return status;
}
