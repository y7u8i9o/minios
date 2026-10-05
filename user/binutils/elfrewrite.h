#pragma once
/* The ELF rewriter of objcopy and strip.  It copies an ELF64 file or the
 * ELF members of an archive and applies the options of the two programs:
 * the removal of sections and symbols, the stripping of debug sections and
 * the addition of sections.  Executables and shared objects retain their
 * program headers and the file offsets of all allocated sections.  The
 * section header table and the other sections follow the allocated
 * contents.  Relocatable objects are laid out again. */
#include "lib/binutils.h"

enum elfrewrite_strip {
    ELFREWRITE_STRIP_NONE,
    ELFREWRITE_STRIP_DEBUG,     /* debug sections and file symbols */
    ELFREWRITE_STRIP_UNNEEDED,  /* debug sections and symbols that no relocation uses */
    ELFREWRITE_STRIP_ALL,       /* debug sections and all symbols */
};

/* A list of strings or of wildcard patterns with * and ?. */
struct elfrewrite_list {
    const char **items;
    size_t count;
};

struct elfrewrite_added {
    const char *section;        /* the name of the new section */
    uint8_t *data;              /* the contents, read from a file */
    size_t size;
};

struct elfrewrite_options {
    int strip;                          /* an enum elfrewrite_strip */
    struct elfrewrite_list remove_sections;
    struct elfrewrite_list only_sections;
    struct elfrewrite_list strip_symbols;
    struct elfrewrite_list retain_symbols;
    struct elfrewrite_added *added;
    size_t added_count;
    int preserve_dates;
    const char *output;                 /* -o of strip */
    const char *input_format;           /* -I of objcopy */
    const char *output_format;          /* -O of objcopy */
};

/* The options that objcopy and strip share.  The two programs give the
 * letter -S different meanings, so each program parses its own letters
 * with getopt_long and passes one of these codes to elfrewrite_apply. */
enum elfrewrite_option {
    ELFREWRITE_OPT_STRIP_ALL = 256,
    ELFREWRITE_OPT_STRIP_DEBUG,
    ELFREWRITE_OPT_STRIP_UNNEEDED,
    ELFREWRITE_OPT_PRESERVE_DATES,
    ELFREWRITE_OPT_REMOVE_SECTION,      /* argument: a section or a pattern */
    ELFREWRITE_OPT_ONLY_SECTION,        /* argument: a section or a pattern */
    ELFREWRITE_OPT_STRIP_SYMBOL,        /* argument: a symbol or a pattern */
    ELFREWRITE_OPT_RETAIN_SYMBOL,       /* argument: a symbol or a pattern, -K */
    ELFREWRITE_OPT_ADD_SECTION,         /* argument: SECTION=FILE */
    ELFREWRITE_OPT_OUTPUT,              /* argument: the output file of strip -o */
    ELFREWRITE_OPT_INPUT_FORMAT,        /* argument: a bfd name */
    ELFREWRITE_OPT_OUTPUT_FORMAT,       /* argument: a bfd name */
};

/* elfrewrite_apply records the option code with its argument in o.  The
 * result is 0, or -1 after a message about an invalid argument. */
int elfrewrite_apply(struct elfrewrite_options *o, int code, const char *arg);

/* elfrewrite_image rewrites the ELF file f into a new image.  *out
 * receives memory that the caller frees.  The result is 0 or a negative
 * errno.  After a failure elfrewrite_message returns the reason. */
int elfrewrite_image(const struct elffile *f, const struct elfrewrite_options *o, uint8_t **out, size_t *size);
/* elfrewrite_binary writes the loadable contents of f as objcopy -O
 * binary does: the sections with contents at the distance of their load
 * addresses from the lowest load address. */
int elfrewrite_binary(const struct elffile *f, const struct elfrewrite_options *o, uint8_t **out, size_t *size);
/* elfrewrite_message returns the text of the last failure. */
const char *elfrewrite_message(void);

/* elfrewrite_path reads the file in, which is an ELF file or an archive of
 * ELF files, applies the options and writes the result to output, or over
 * in when output is NULL.  With binary set the output is a raw image.  The
 * messages go to the standard error.  The result is 0, or 1 after an
 * error. */
int elfrewrite_path(const char *in, const char *output, const struct elfrewrite_options *o, int binary);
