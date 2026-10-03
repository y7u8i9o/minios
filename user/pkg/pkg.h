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
#define PKG_MAX_CONFIG 32
/* The package format this installer writes and accepts. Format 2 names
 * every file relative to the installation root. */
#define PKG_FORMAT 2

struct pkg_dep { char name[PKG_NAME_MAX]; char op[3]; char version[PKG_VERSION_MAX]; };
struct pkg_lib { char soname[PKG_NAME_MAX]; int abi; };
struct pkg_launcher { char title[64]; char command[128]; };
struct pkg_mime_type { char type[48]; char extensions[64]; };
struct pkg_handler { char type[48]; char command[128]; };

struct manifest {
    int format;                 /* 0 when the manifest has no format line */
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
    char config[PKG_MAX_CONFIG][128]; int nconfig;
    char unchecked[PKG_MAX_CONFIG][128]; int nunchecked;  /* fnmatch patterns, "*" matching "/" too */
    char kernel[128];           /* the kernel file the boot loader loads (boot.c) */
    char bios_stage[128];       /* the BIOS stage of the boot loader (boot.c) */
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
int manifest_is_config(const struct manifest *m, const char *rel);
int manifest_is_unchecked(const struct manifest *m, const char *rel);

/* archive.c: a gzip compressed ustar archive held in memory. */
struct member {
    char path[PKG_PATH_MAX];    /* as named in the archive */
    int dir;
    uint32_t mode;              /* without setuid and setgid unless uid is 0 */
    uint32_t uid, gid;
    int link;                   /* a symbolic link to target */
    char target[101];
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
int tarw_add_link(struct tar_writer *w, const char *path, const char *target, time_t mtime);
int tarw_finish(struct tar_writer *w, const char *outpath);

/* elf.c: what the loader will ask of an ELF file. */
typedef int (*elf_symbol_fn)(const char *name, void *arg);
int elf_is(const uint8_t *data, size_t len);
int elf_needed(const uint8_t *data, size_t len, char (*names)[PKG_NAME_MAX], int max);
int elf_undefined(const uint8_t *data, size_t len, elf_symbol_fn fn, void *arg);
int elf_defines(const uint8_t *data, size_t len, const char *name);
int elf_defined(const uint8_t *data, size_t len, elf_symbol_fn fn, void *arg);
/* The machine name of an ELF file in the form of uname -m: x86_64,
 * aarch64, or unknown. */
const char *elf_arch(const uint8_t *data, size_t len);

/* db.c: the records under <root>/var/lib/pkg and the tables the desktop
 * reads. A file or directory of a record carries the mode and owner the
 * archive gave it, and a file its size and SHA-256 digest. A symbolic link
 * is a file whose mode has the type bits S_IFLNK, whose size is the length
 * of its target and whose digest is that of the target. */
struct owned {
    char path[PKG_PATH_MAX];
    uint32_t mode, uid, gid;
    size_t size;
    uint8_t sha256[32];
};
struct record {
    struct owned *files; int nfiles, cap_files;
    struct owned *dirs; int ndirs, cap_dirs;
};
extern const char *root;
extern const char *target_arch;
int db_lock(void);
void db_unlock(void);
int db_read(const char *name, struct manifest *m);
int db_names(char (**names)[PKG_NAME_MAX]);
int db_read_record(const char *name, struct record *r);
int db_write(const char *name, const struct manifest *m, const struct record *r);
int db_delete(const char *name);
int db_owner(const char *relpath, char *owner, size_t n);
void db_owner_reset(void);
int db_write_tables(void);
void db_path(char *buf, size_t n, const char *rel);
int record_add_file(struct record *r, const char *path, uint32_t mode, uint32_t uid, uint32_t gid,
                    size_t size, const uint8_t sha256[32]);
int record_add_dir(struct record *r, const char *path, uint32_t mode, uint32_t uid, uint32_t gid);
int record_has_dir(const struct record *r, const char *path);
const struct owned *record_file(const struct record *r, const char *path);
void record_free(struct record *r);
int read_file(const char *path, uint8_t **data, size_t *len);
int write_file(const char *path, const uint8_t *data, size_t len);
int write_file_mode(const char *path, const uint8_t *data, size_t len, uint32_t mode);
int mkdir_all(const char *path);
void path_join(char *buf, size_t n, const char *dir, const char *rel);
void root_path(char *buf, size_t n, const char *rel);
int file_sha256(const char *path, uint8_t sha256[32]);

/* boot.c: the boot loader configuration that pkg writes from the
 * installed kernel package and /etc/kernel/cmdline, and the BIOS stage
 * that it installs again when the boot loader package changes. */
int boot_write_config(void);
int boot_bios_install(void);
void boot_save_previous(const char *target);

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
extern const char *keys_path;
int config_read(struct repo_config *c);
int index_load(const struct repo_config *c, struct index *ix, int quiet);
void index_free(struct index *ix);
const struct index_entry *index_best(const struct index *ix, const char *name, const struct pkg_dep *want);
const struct index_entry *index_find(const struct index *ix, const char *name, const char *version);
const struct index_entry *index_provider(const struct index *ix, const struct pkg_lib *lib);
int repo_fetch(const struct repo_config *c, const struct index_entry *e, const char *dest);
int repo_local_archive(const struct repo_config *c, const struct index_entry *e, char *path, size_t n);
int file_matches(const struct index_entry *e, const char *file, const char *label, const char *repo,
                 char *err, size_t errlen);
int config_present(void);
int cmd_update(void);
int cmd_search(int argc, char **argv);
