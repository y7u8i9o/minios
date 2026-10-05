/* objcopy: copy an ELF file or the ELF members of an archive and
 * transform it on the way (elfrewrite.c).  The output format binary writes
 * the loadable contents as a raw image.
 *
 *     objcopy [options] infile [outfile]
 */
#include "elfrewrite.h"
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *to)
{
    fprintf(to,
            "Usage: %s [option(s)] in-file [out-file]\n"
            " Copies a binary file, possibly transforming it in the process\n"
            " The options are:\n"
            "  -I --input-target <bfdname>      Assume input file is in format <bfdname>\n"
            "  -O --output-target <bfdname>     Create an output file in format <bfdname>\n"
            "  -j --only-section <name>         Only copy section <name> into the output\n"
            "     --add-section <name>=<file>   Add section <name> found in <file> to output\n"
            "  -R --remove-section <name>       Remove section <name> from the output\n"
            "  -S --strip-all                   Remove all symbol and relocation information\n"
            "  -g --strip-debug                 Remove all debugging symbols & sections\n"
            "     --strip-unneeded              Remove all symbols not needed by relocations\n"
            "  -N --strip-symbol <name>         Do not copy symbol <name>\n"
            "  -K --keep-symbol <name>          Do not strip symbol <name>\n"
            "  -p --preserve-dates              Copy modified/access timestamps to the output\n"
            "  -h --help                        Display this output\n"
            "  -V --version                     Display this program's version number\n",
            bu_program);
}

static int format_valid(const char *name)
{
    return strcmp(name, "binary") == 0 || strcmp(name, "default") == 0 || strcmp(name, "elf64-little") == 0 ||
           strcmp(name, "elf64-x86-64") == 0 || strcmp(name, "elf64-littleaarch64") == 0;
}

static const struct option longopts[] = {
    { "strip-all", no_argument, NULL, 'S' },
    { "strip-debug", no_argument, NULL, 'g' },
    { "strip-unneeded", no_argument, NULL, ELFREWRITE_OPT_STRIP_UNNEEDED },
    { "preserve-dates", no_argument, NULL, 'p' },
    { "remove-section", required_argument, NULL, 'R' },
    { "only-section", required_argument, NULL, 'j' },
    { "strip-symbol", required_argument, NULL, 'N' },
    { "keep-symbol", required_argument, NULL, 'K' },
    { "add-section", required_argument, NULL, ELFREWRITE_OPT_ADD_SECTION },
    { "input-target", required_argument, NULL, 'I' },
    { "output-target", required_argument, NULL, 'O' },
    { "help", no_argument, NULL, 'h' },
    { "version", no_argument, NULL, 'V' },
    { NULL, 0, NULL, 0 },
};

/* The letters of objcopy.  -S is --strip-all here, unlike in strip. */
static int option_code(int c)
{
    switch (c) {
    case 'S': return ELFREWRITE_OPT_STRIP_ALL;
    case 'g': return ELFREWRITE_OPT_STRIP_DEBUG;
    case 'p': return ELFREWRITE_OPT_PRESERVE_DATES;
    case 'R': return ELFREWRITE_OPT_REMOVE_SECTION;
    case 'j': return ELFREWRITE_OPT_ONLY_SECTION;
    case 'N': return ELFREWRITE_OPT_STRIP_SYMBOL;
    case 'K': return ELFREWRITE_OPT_RETAIN_SYMBOL;
    case 'I': return ELFREWRITE_OPT_INPUT_FORMAT;
    case 'O': return ELFREWRITE_OPT_OUTPUT_FORMAT;
    default: return c;
    }
}

int main(int argc, char **argv)
{
    bu_program = "objcopy";
    struct elfrewrite_options o;
    memset(&o, 0, sizeof o);
    int c;
    while ((c = getopt_long(argc, argv, "SgpR:j:N:K:I:O:hV", longopts, NULL)) != -1) {
        if (c == 'h') {
            usage(stdout);
            return 0;
        }
        if (c == 'V') {
            printf("objcopy (minios binutils)\n");
            return 0;
        }
        if (c == '?' || elfrewrite_apply(&o, option_code(c), optarg) < 0) {
            usage(stderr);
            return 1;
        }
    }
    char **files = argv + optind;
    int nfiles = argc - optind;
    if (nfiles == 0 || nfiles > 2) {
        if (nfiles > 2)
            bu_error(NULL, "too many file arguments");
        usage(stderr);
        return 1;
    }
    for (int k = 0; k < 2; k++) {
        const char *format = k ? o.output_format : o.input_format;
        if (format && !format_valid(format))
            bu_fatal(NULL, "%s: invalid bfd target", format);
    }
    if (o.input_format && strcmp(o.input_format, "binary") == 0)
        bu_fatal(NULL, "the input format binary is not supported");
    int binary = o.output_format && strcmp(o.output_format, "binary") == 0;
    return elfrewrite_path(files[0], nfiles == 2 ? files[1] : NULL, &o, binary);
}
