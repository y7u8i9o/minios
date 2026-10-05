#pragma once
/* The code that the programs of the package binutils share: messages and
 * memory, the reading of files, archives in the System V format with
 * the symbol index of the GNU binutils, and the iteration over the ELF
 * objects of a file or of an archive (docs/design/binutils.md). */
#include <minios/elffile.h>
#include <stddef.h>
#include <stdint.h>

/* ---- messages and memory ---- */

/* bu_program is the name of the program in messages, set by main. */
extern const char *bu_program;
/* bu_error prints "PROGRAM: FILE: MESSAGE" on the standard error, or
 * "PROGRAM: MESSAGE" when file is NULL. */
void bu_error(const char *file, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* bu_fatal prints like bu_error and exits with status 1. */
void bu_fatal(const char *file, const char *fmt, ...) __attribute__((format(printf, 2, 3), noreturn));
/* bu_alloc and bu_strdup end the program when memory is exhausted. */
void *bu_alloc(size_t size);
void *bu_realloc(void *p, size_t size);
char *bu_strdup(const char *s);
char *bu_strndup(const char *s, size_t n);

/* bu_read_file reads the file at path into memory.  The result is 0 or a
 * negative errno.  *data receives memory that the caller frees. */
int bu_read_file(const char *path, uint8_t **data, size_t *size);
/* bu_write_file writes size bytes to a temporary file beside path with
 * the mode and renames it over path.  The result is 0 or a negative
 * errno. */
int bu_write_file(const char *path, const void *data, size_t size, unsigned mode);

/* ---- archives ---- */

struct ar_member {
    char *name;                 /* the decoded name, without the slash */
    long date;
    int uid, gid;
    unsigned mode;
    size_t size;
    const uint8_t *data;        /* the contents, equal to owned unless a caller replaced them */
    uint8_t *owned;             /* the memory of the contents, freed by archive_free */
    size_t offset;              /* the offset of the header in the parsed archive */
    struct ar_member *next;
};

struct archive {
    struct ar_member *members;
    const uint8_t *index;       /* the contents of the "/" member, or NULL */
    size_t index_size;
    long index_date;            /* the date that archive_write gives the index, 0 by default */
};

/* archive_is reports whether data starts with the archive magic. */
int archive_is(const void *data, size_t size);
/* archive_parse builds the member list of the archive in data.  Each
 * member receives a copy of its contents in memory of its own, aligned
 * for the ELF structures.  The index points into data, which must exist
 * as long as the archive.  The result is 0, or -1 after a message to *err
 * of errlen bytes. */
int archive_parse(struct archive *a, const uint8_t *data, size_t size, char *err, size_t errlen);
/* archive_free releases the member list and the owned memory. */
void archive_free(struct archive *a);
/* archive_append adds m at the end of the member list. */
void archive_append(struct archive *a, struct ar_member *m);
/* archive_index_count returns the number of entries of the parsed symbol
 * index, and archive_index_entry the name and the member header offset of
 * an entry.  Both report 0 for an archive without an index. */
size_t archive_index_count(const struct archive *a);
int archive_index_entry(const struct archive *a, size_t i, const char **name, size_t *offset);
/* archive_write writes the archive to path through a temporary file.
 * With index set, a symbol index "/" lists the defined global symbols of
 * the ELF members in the order of the members and of their symbol
 * tables, as GNU ar and ranlib write it.  The header of the index carries
 * the date index_date.  Long names follow in the "//" table.  An existing
 * archive retains its mode, and a new archive receives the mode 0644.
 * The result is 0, or a negative errno. */
int archive_write(const struct archive *a, const char *path, int index);
/* archive_index_symbol reports whether the symbol belongs in the index. */
int archive_index_symbol(const Elf64_Sym *sym);

/* ---- objects ---- */

/* bu_object_fn receives one ELF object.  member is the name of the
 * archive member, or NULL for a plain file. */
typedef int (*bu_object_fn)(const char *path, const char *member, const struct elffile *f, void *arg);
/* bu_archive_fn receives an archive before its members. */
typedef int (*bu_archive_fn)(const char *path, const struct archive *a, void *arg);
/* bu_other_fn receives an archive member that is no ELF64 file, before
 * the message about the member.  readelf prints the heading of the
 * member there. */
typedef void (*bu_other_fn)(const char *path, const char *member, void *arg);

/* bu_for_each_object reads path and calls fn for the ELF file or for
 * every ELF member of the archive.  archive_fn and other_fn may be NULL.
 * A member that is no ELF64 file gives the message "file format not
 * recognized".  The result is 0 when every object was read and every call
 * returned 0, else 1. */
int bu_for_each_object(const char *path, bu_archive_fn archive_fn, bu_object_fn fn, bu_other_fn other_fn,
                       void *arg);
