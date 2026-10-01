/* The package installer (docs/design/packages.md): shared declarations. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define PKG_NAME_MAX 64
#define PKG_VERSION_MAX 32
#define PKG_PATH_MAX 256
#define PKG_MAX_DEPS 32
#define PKG_MAX_LIBS 32
#define PKG_MAX_LAUNCHERS 8
#define PKG_MAX_MIME 16

struct pkg_dep { char name[PKG_NAME_MAX]; char op[3]; char version[PKG_VERSION_MAX]; };
struct pkg_lib { char soname[PKG_NAME_MAX]; int abi; };
struct pkg_launcher { char title[64]; char command[128]; };
struct pkg_mime_type { char type[48]; char extensions[64]; };
struct pkg_handler { char type[48]; char command[128]; };

struct manifest {
    char name[PKG_NAME_MAX], version[PKG_VERSION_MAX], summary[160];
    char arch[16];              /* machine of the ELF files, empty when there are none */
    struct pkg_dep deps[PKG_MAX_DEPS]; int ndeps;
    char conflicts[PKG_MAX_DEPS][PKG_NAME_MAX]; int nconflicts;
    struct pkg_lib provides[PKG_MAX_LIBS]; int nprovides;
    struct pkg_lib needs[PKG_MAX_LIBS]; int nneeds;
    struct pkg_launcher launchers[PKG_MAX_LAUNCHERS]; int nlaunchers;
    struct pkg_mime_type types[PKG_MAX_MIME]; int ntypes;
    struct pkg_handler handlers[PKG_MAX_MIME]; int nhandlers;
    char icon[128];
};

/* The machine name of the running system, as uname -m prints it (pkg.c). */
const char *system_arch(void);

/* manifest.c. Errors are described in err. */
int manifest_parse(struct manifest *m, const char *text, size_t len, char *err, size_t errlen);
int manifest_read(struct manifest *m, const char *path, char *err, size_t errlen);
void manifest_write(FILE *f, const struct manifest *m);
int name_valid(const char *name);
int version_valid(const char *v);
int version_cmp(const char *a, const char *b);
int dep_satisfied(const struct pkg_dep *d, const char *version);
const struct pkg_lib *manifest_provides(const struct manifest *m, const char *soname);

/* archive.c: a gzip compressed ustar archive held in memory. */
struct member {
    char path[PKG_PATH_MAX];    /* as named in the archive */
    int dir;
    uint32_t mode;
    time_t mtime;
    size_t size;
    const uint8_t *data;
};
struct archive { uint8_t *data; size_t len, at; };
int archive_load(struct archive *a, const char *path, char *err, size_t errlen);
int archive_next(struct archive *a, struct member *m, char *err, size_t errlen);
void archive_rewind(struct archive *a);
void archive_free(struct archive *a);
struct tar_writer { uint8_t *data; size_t len, cap; };
int tarw_add(struct tar_writer *w, const char *path, int dir, uint32_t mode, time_t mtime, const uint8_t *data, size_t size);
int tarw_finish(struct tar_writer *w, const char *outpath);

/* elf.c: what the loader will ask of an ELF file. */
typedef int (*elf_symbol_fn)(const char *name, void *arg);
int elf_is(const uint8_t *data, size_t len);
int elf_needed(const uint8_t *data, size_t len, char (*names)[PKG_NAME_MAX], int max);
int elf_undefined(const uint8_t *data, size_t len, elf_symbol_fn fn, void *arg);
int elf_defines(const uint8_t *data, size_t len, const char *name);
/* The machine name of an ELF file in the form of uname -m: x86_64,
 * aarch64, or unknown. */
const char *elf_arch(const uint8_t *data, size_t len);

/* db.c: the records under <prefix>/lib/pkg and the tables the desktop reads. */
struct owned { char path[PKG_PATH_MAX]; size_t size; uint32_t crc; };
struct record {
    struct owned *files; int nfiles, cap_files;
    char (*dirs)[PKG_PATH_MAX]; int ndirs, cap_dirs;
};
extern const char *prefix;
extern const char *sysroot;
int db_lock(void);
void db_unlock(void);
int db_read(const char *name, struct manifest *m);
int db_names(char (**names)[PKG_NAME_MAX]);
int db_read_record(const char *name, struct record *r);
int db_write(const char *name, const struct manifest *m, const struct record *r);
int db_delete(const char *name);
int db_owner(const char *relpath, char *owner, size_t n);
int db_write_tables(void);
int record_add_file(struct record *r, const char *path, size_t size, uint32_t crc);
int record_add_dir(struct record *r, const char *path);
int record_has_dir(const struct record *r, const char *path);
void record_free(struct record *r);
int system_abi(const char *soname);
int read_file(const char *path, uint8_t **data, size_t *len);
int write_file(const char *path, const uint8_t *data, size_t len);
int write_file_mode(const char *path, const uint8_t *data, size_t len, uint32_t mode);
int mkdir_all(const char *path);
void path_join(char *buf, size_t n, const char *dir, const char *rel);

/* repo.c verifies repository indexes and fetches archives over HTTP. The
 * timeout of a configuration bounds, in seconds, the connection and each
 * wait for data. An index entry holds the manifest keys of the archive in
 * m, its path relative to the repository URL, its size and digest, and
 * the position of its repository in repo_config.repos. */
#define PKG_MAX_REPOS 8
#define PKG_URL_MAX 512
struct repo { char name[PKG_NAME_MAX]; char url[PKG_URL_MAX]; };
struct repo_config {
    struct repo repos[PKG_MAX_REPOS];
    int nrepos;
    int timeout;
};
struct index_entry {
    struct manifest m;
    char path[PKG_PATH_MAX];
    long long size;
    uint8_t sha256[32];
    int repo;
};
struct index { struct index_entry *entries; int n, cap; };
extern const char *config_path;
int config_read(struct repo_config *c);
int index_load(const struct repo_config *c, struct index *ix, int quiet);
void index_free(struct index *ix);
const struct index_entry *index_best(const struct index *ix, const char *name, const struct pkg_dep *want);
const struct index_entry *index_find(const struct index *ix, const char *name, const char *version);
const struct index_entry *index_provider(const struct index *ix, const struct pkg_lib *lib);
int repo_fetch(const struct repo_config *c, const struct index_entry *e, const char *dest);
int file_matches(const struct index_entry *e, const char *file, const char *label, const char *repo,
                 char *err, size_t errlen);
int config_present(void);
int cmd_update(void);
int cmd_search(int argc, char **argv);
