/* pkg: install, remove, list, verify and build packages
 * (docs/design/packages.md). Every check of an installation runs before
 * anything is written. */
#include "pkg.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <minios/gzip.h>

#define MAX_PENDING 32
#define MAX_LIBS 64
#define MAX_SYMBOLS 4096

struct pending {
    const char *file;
    struct archive ar;
    struct manifest m;
    int upgrade;                /* another version is installed */
    int skip;                   /* the same version is installed */
    int done;                   /* ordering */
    int fetched;                /* The archive came from a repository. */
};

static struct pending pend[MAX_PENDING];
static int npend;
static char (*installed)[PKG_NAME_MAX];
static int ninstalled;
static struct manifest *installed_m;
static int force;

static void usage(void)
{
    fputs("usage: pkg [--prefix DIR] [--root DIR] [--config FILE] command...\n"
          "       pkg install FILE|NAME[-VERSION]...\n"
          "       pkg check FILE|NAME[-VERSION]...\n"
          "       pkg update\n"
          "       pkg search [PATTERN]\n"
          "       pkg upgrade [NAME...]\n"
          "       pkg remove [--force] NAME...\n"
          "       pkg list\n"
          "       pkg info NAME|FILE\n"
          "       pkg verify [NAME...]\n"
          "       pkg build DIR [FILE]\n", stderr);
    exit(2);
}

static int error(const char *pkg, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static int error(const char *pkg, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (pkg)
        fprintf(stderr, "pkg: %s: ", pkg);
    else
        fputs("pkg: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    return -1;
}

static void load_installed(void)
{
    free(installed);
    free(installed_m);
    ninstalled = db_names(&installed);
    installed_m = calloc((size_t)(ninstalled > 0 ? ninstalled : 1), sizeof *installed_m);
    for (int i = 0; i < ninstalled; i++)
        if (db_read(installed[i], &installed_m[i]) < 0)
            memset(&installed_m[i], 0, sizeof installed_m[i]);
}

static const struct manifest *installed_manifest(const char *name)
{
    for (int i = 0; i < ninstalled; i++)
        if (strcmp(installed[i], name) == 0 && installed_m[i].name[0])
            return &installed_m[i];
    return NULL;
}

static struct pending *pending_named(const char *name)
{
    for (int i = 0; i < npend; i++)
        if (strcmp(pend[i].m.name, name) == 0)
            return &pend[i];
    return NULL;
}

/* ---- members ---- */

/* The prefix relative path of a member, or NULL for the manifest and
 * the files/ directory itself; -1 for a member outside files/. */
static int member_rel(const struct member *m, const char **rel)
{
    *rel = NULL;
    if (strcmp(m->path, "manifest") == 0 || strcmp(m->path, "files") == 0)
        return 0;
    if (strncmp(m->path, "files/", 6) != 0)
        return -1;
    const char *p = m->path + 6;
    for (const char *s = p; *s; ) {
        const char *e = strchr(s, '/');
        size_t n = e ? (size_t)(e - s) : strlen(s);
        if (n == 0 || (n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.'))
            return -1;
        s += n;
        if (*s == '/') s++;
    }
    *rel = p;
    return 0;
}

/* Reads the manifest and checks the member list of an archive. */
static int open_package(struct pending *p)
{
    char err[256];
    struct member m;
    if (archive_load(&p->ar, p->file, err, sizeof err) < 0)
        return error(NULL, "%s", err);
    int r = archive_next(&p->ar, &m, err, sizeof err);
    if (r < 0)
        return error(p->file, "%s", err);
    if (r == 0 || strcmp(m.path, "manifest") != 0 || m.dir)
        return error(p->file, "the first member is not the manifest");
    if (manifest_parse(&p->m, (const char *)m.data, m.size, err, sizeof err) < 0)
        return error(p->file, "%s", err);
    while ((r = archive_next(&p->ar, &m, err, sizeof err)) > 0) {
        const char *rel;
        if (member_rel(&m, &rel) < 0)
            return error(p->m.name, "member %s is outside files/", m.path);
        if (strcmp(m.path, "manifest") == 0)
            return error(p->m.name, "the manifest appears twice");
    }
    if (r < 0)
        return error(p->m.name, "%s", err);
    archive_rewind(&p->ar);
    return 0;
}

/* ---- the library rule ---- */

struct lib {
    char soname[PKG_NAME_MAX];
    int abi;
    char from[PKG_NAME_MAX];    /* "the system" or a package name */
    uint8_t *owned;
    const uint8_t *data;
    size_t len;
};
static struct lib libs[MAX_LIBS];
static int nlibs;

static const struct member *find_member(struct pending *p, const char *rel, struct member *out)
{
    char err[128];
    archive_rewind(&p->ar);
    while (archive_next(&p->ar, out, err, sizeof err) > 0) {
        const char *r;
        if (member_rel(out, &r) == 0 && r && strcmp(r, rel) == 0 && !out->dir) {
            archive_rewind(&p->ar);
            return out;
        }
    }
    archive_rewind(&p->ar);
    return NULL;
}

/* Where a soname comes from: a pending package, an installed package, or
 * the system, loaded once. NULL when nothing provides it. */
static struct lib *find_library(const char *soname)
{
    for (int i = 0; i < nlibs; i++)
        if (strcmp(libs[i].soname, soname) == 0)
            return &libs[i];
    if (nlibs == MAX_LIBS)
        return NULL;
    struct lib *l = &libs[nlibs];
    memset(l, 0, sizeof *l);
    strlcpy(l->soname, soname, sizeof l->soname);
    char rel[PKG_PATH_MAX], path[PKG_PATH_MAX];
    snprintf(rel, sizeof rel, "lib/%s", soname);
    for (int i = 0; i < npend; i++) {
        const struct pkg_lib *pl = manifest_provides(&pend[i].m, soname);
        struct member m;
        if (!pl || pend[i].skip)
            continue;
        if (!find_member(&pend[i], rel, &m))
            return NULL;
        l->abi = pl->abi;
        l->data = m.data;
        l->len = m.size;
        strlcpy(l->from, pend[i].m.name, sizeof l->from);
        nlibs++;
        return l;
    }
    for (int i = 0; i < ninstalled; i++) {
        const struct pkg_lib *pl = manifest_provides(&installed_m[i], soname);
        if (!pl || pending_named(installed[i]))
            continue;
        path_join(path, sizeof path, prefix, rel);
        if (read_file(path, &l->owned, &l->len) < 0)
            return NULL;
        l->data = l->owned;
        l->abi = pl->abi;
        strlcpy(l->from, installed[i], sizeof l->from);
        nlibs++;
        return l;
    }
    snprintf(path, sizeof path, "%s/lib/%s", sysroot, soname);
    if (read_file(path, &l->owned, &l->len) == 0) {
        l->data = l->owned;
        l->abi = system_abi(soname);
        strlcpy(l->from, "the system", sizeof l->from);
        nlibs++;
        return l;
    }
    return NULL;
}

struct symbols { char (*names)[PKG_NAME_MAX]; int n; };

static int collect_symbol(const char *name, void *arg)
{
    struct symbols *s = arg;
    if (s->n >= MAX_SYMBOLS || strlen(name) >= PKG_NAME_MAX)
        return 0;
    for (int i = 0; i < s->n; i++)
        if (strcmp(s->names[i], name) == 0)
            return 0;
    strlcpy(s->names[s->n++], name, PKG_NAME_MAX);
    return 0;
}

/* The loader's view of one ELF file of package name: every library in the
 * transitive DT_NEEDED closure must exist and, together, define every
 * symbol the file leaves undefined. */
const char *system_arch(void)
{
    static struct utsname u;
    if (!u.machine[0] && uname(&u) < 0)
        strlcpy(u.machine, "unknown", sizeof u.machine);
    return u.machine;
}

static int check_elf(const char *name, const char *rel, const uint8_t *data, size_t len)
{
    /* A program or library for another machine cannot be loaded. */
    const char *arch = elf_arch(data, len);
    if (strcmp(arch, system_arch()) != 0)
        return error(name, "%s is built for %s, and the system is %s", rel, arch, system_arch());
    char (*needed)[PKG_NAME_MAX] = calloc(MAX_LIBS, PKG_NAME_MAX);
    struct symbols syms = { calloc(MAX_SYMBOLS, PKG_NAME_MAX), 0 };
    int nneeded = elf_needed(data, len, needed, MAX_LIBS), r = 0;
    if (nneeded < 0)
        nneeded = 0;                    /* a static program or a relocatable object */
    if (nneeded > MAX_LIBS)
        nneeded = MAX_LIBS;
    for (int i = 0; i < nneeded && r == 0; i++) {
        struct lib *l = find_library(needed[i]);
        if (!l) {
            r = error(name, "%s needs %s, which nothing provides", rel, needed[i]);
            break;
        }
        char (*more)[PKG_NAME_MAX] = calloc(MAX_LIBS, PKG_NAME_MAX);
        int nmore = elf_needed(l->data, l->len, more, MAX_LIBS);
        for (int j = 0; j < nmore && j < MAX_LIBS; j++) {
            int seen = 0;
            for (int k = 0; k < nneeded; k++)
                if (strcmp(needed[k], more[j]) == 0)
                    seen = 1;
            if (!seen && nneeded < MAX_LIBS)
                strlcpy(needed[nneeded++], more[j], PKG_NAME_MAX);
        }
        free(more);
    }
    if (r == 0 && nneeded)
        elf_undefined(data, len, collect_symbol, &syms);
    for (int i = 0; i < syms.n && r == 0; i++) {
        int defined = 0;
        for (int j = 0; j < nneeded && !defined; j++) {
            struct lib *l = find_library(needed[j]);
            if (l && elf_defines(l->data, l->len, syms.names[i]))
                defined = 1;
        }
        if (!defined)
            r = error(name, "%s: undefined symbol %s", rel, syms.names[i]);
    }
    free(needed);
    free(syms.names);
    return r;
}

static int check_needs(const struct manifest *m)
{
    for (int i = 0; i < m->nneeds; i++) {
        const struct pkg_lib *n = &m->needs[i];
        struct lib *l = find_library(n->soname);
        if (!l)
            return error(m->name, "needs %s, which nothing provides", n->soname);
        if (l->abi != n->abi)
            return error(m->name, "needs %s ABI %d, %s has %d", n->soname, n->abi, l->from, l->abi);
    }
    return 0;
}

/* Every ELF file in a pending package. */
static int check_pending_elves(struct pending *p)
{
    char err[128];
    struct member m;
    int r;
    archive_rewind(&p->ar);
    while ((r = archive_next(&p->ar, &m, err, sizeof err)) > 0) {
        const char *rel;
        if (member_rel(&m, &rel) < 0 || !rel || m.dir || !elf_is(m.data, m.size))
            continue;
        if (check_elf(p->m.name, rel, m.data, m.size) < 0) {
            archive_rewind(&p->ar);
            return -1;
        }
    }
    archive_rewind(&p->ar);
    return 0;
}

/* Every ELF file of an installed package, read from disk. */
static int check_installed_elves(const char *name)
{
    struct record rec;
    if (db_read_record(name, &rec) < 0)
        return 0;
    int r = 0;
    for (int i = 0; i < rec.nfiles && r == 0; i++) {
        char path[PKG_PATH_MAX];
        uint8_t *data;
        size_t len;
        path_join(path, sizeof path, prefix, rec.files[i].path);
        if (read_file(path, &data, &len) < 0)
            continue;
        if (elf_is(data, len))
            r = check_elf(name, rec.files[i].path, data, len);
        free(data);
    }
    record_free(&rec);
    return r;
}

/* True if the installed package name is built for another machine: its
 * manifest names another arch, or, for a record written before the arch
 * key existed, one of its ELF files is for another machine. A data volume
 * moved from x86_64 to aarch64 contains such packages. */
static int installed_foreign(const char *name, const struct manifest *im)
{
    if (im->arch[0])
        return strcmp(im->arch, system_arch()) != 0;
    struct record rec;
    if (db_read_record(name, &rec) < 0)
        return 0;
    int foreign = 0;
    for (int i = 0; i < rec.nfiles && !foreign; i++) {
        char path[PKG_PATH_MAX];
        uint8_t *data;
        size_t len;
        path_join(path, sizeof path, prefix, rec.files[i].path);
        if (read_file(path, &data, &len) < 0)
            continue;
        if (elf_is(data, len) && strcmp(elf_arch(data, len), system_arch()) != 0)
            foreign = 1;
        free(data);
    }
    record_free(&rec);
    return foreign;
}

/* ---- the checks of install ---- */

static int check_all(void)
{
    /* Names, versions and what is installed already. */
    for (int i = 0; i < npend; i++) {
        struct pending *p = &pend[i];
        if (p->m.arch[0] && strcmp(p->m.arch, system_arch()) != 0)
            return error(p->m.name, "the package is built for %s, and the system is %s",
                         p->m.arch, system_arch());
        for (int j = 0; j < i; j++)
            if (strcmp(pend[j].m.name, p->m.name) == 0)
                return error(p->m.name, "named twice on the command line");
        const struct manifest *im = installed_manifest(p->m.name);
        if (im) {
            if (strcmp(im->version, p->m.version) == 0 && !installed_foreign(p->m.name, im)) {
                printf("%s %s is installed already\n", p->m.name, p->m.version);
                p->skip = 1;
            } else {
                p->upgrade = 1;
            }
        }
    }
    /* Conflicts, in both directions, against installed and pending packages. */
    for (int i = 0; i < npend; i++) {
        struct pending *p = &pend[i];
        if (p->skip)
            continue;
        for (int j = 0; j < p->m.nconflicts; j++) {
            const char *c = p->m.conflicts[j];
            const struct pending *q = pending_named(c);
            if ((q && !q->skip) || (installed_manifest(c) && !q))
                return error(p->m.name, "conflicts with %s", c);
        }
        for (int j = 0; j < ninstalled; j++) {
            if (pending_named(installed[j]))
                continue;
            for (int k = 0; k < installed_m[j].nconflicts; k++)
                if (strcmp(installed_m[j].conflicts[k], p->m.name) == 0)
                    return error(p->m.name, "%s conflicts with it", installed[j]);
        }
        for (int j = 0; j < npend; j++)
            for (int k = 0; k < pend[j].m.nconflicts; k++)
                if (j != i && !pend[j].skip && strcmp(pend[j].m.conflicts[k], p->m.name) == 0)
                    return error(p->m.name, "%s conflicts with it", pend[j].m.name);
    }
    /* Dependencies. */
    for (int i = 0; i < npend; i++) {
        struct pending *p = &pend[i];
        if (p->skip)
            continue;
        for (int j = 0; j < p->m.ndeps; j++) {
            const struct pkg_dep *d = &p->m.deps[j];
            const struct pending *q = pending_named(d->name);
            const struct manifest *dm = q ? &q->m : installed_manifest(d->name);
            if (!dm)
                return error(p->m.name, "depends on %s, which is not installed", d->name);
            if (!dep_satisfied(d, dm->version))
                return error(p->m.name, "depends on %s %s %s, found %s", d->name, d->op, d->version, dm->version);
        }
    }
    /* Installed packages that depend on a version being replaced. */
    for (int i = 0; i < npend; i++) {
        struct pending *p = &pend[i];
        if (!p->upgrade)
            continue;
        for (int j = 0; j < ninstalled; j++) {
            if (pending_named(installed[j]))
                continue;
            for (int k = 0; k < installed_m[j].ndeps; k++) {
                const struct pkg_dep *d = &installed_m[j].deps[k];
                if (strcmp(d->name, p->m.name) == 0 && !dep_satisfied(d, p->m.version))
                    return error(p->m.name, "%s depends on %s %s %s, this is %s", installed[j], d->name, d->op, d->version, p->m.version);
            }
        }
    }
    /* Libraries: a soname provided by a package may not exist in /lib. */
    for (int i = 0; i < npend; i++) {
        struct pending *p = &pend[i];
        if (p->skip)
            continue;
        for (int j = 0; j < p->m.nprovides; j++) {
            char path[PKG_PATH_MAX], rel[PKG_PATH_MAX];
            struct member m;
            struct stat st;
            snprintf(path, sizeof path, "%s/lib/%s", sysroot, p->m.provides[j].soname);
            if (stat(path, &st) == 0)
                return error(p->m.name, "provides %s, which is a system library", p->m.provides[j].soname);
            snprintf(rel, sizeof rel, "lib/%s", p->m.provides[j].soname);
            if (!find_member(p, rel, &m))
                return error(p->m.name, "provides %s but does not contain lib/%s", p->m.provides[j].soname, p->m.provides[j].soname);
            for (int k = 0; k < ninstalled; k++)
                if (!pending_named(installed[k]) && manifest_provides(&installed_m[k], p->m.provides[j].soname))
                    return error(p->m.name, "provides %s, which %s provides already", p->m.provides[j].soname, installed[k]);
            for (int k = 0; k < npend; k++)
                if (k != i && !pend[k].skip && manifest_provides(&pend[k].m, p->m.provides[j].soname))
                    return error(p->m.name, "provides %s, which %s provides as well", p->m.provides[j].soname, pend[k].m.name);
        }
    }
    for (int i = 0; i < npend; i++) {
        if (pend[i].skip)
            continue;
        if (check_needs(&pend[i].m) < 0 || check_pending_elves(&pend[i]) < 0)
            return -1;
    }
    /* Installed packages that load a library a pending package replaces. */
    for (int i = 0; i < npend; i++) {
        struct pending *p = &pend[i];
        if (p->skip || !p->m.nprovides)
            continue;
        for (int j = 0; j < ninstalled; j++) {
            if (pending_named(installed[j]))
                continue;
            int affected = 0;
            for (int k = 0; k < installed_m[j].nneeds; k++)
                if (manifest_provides(&p->m, installed_m[j].needs[k].soname))
                    affected = 1;
            if (affected && (check_needs(&installed_m[j]) < 0 || check_installed_elves(installed[j]) < 0))
                return -1;
        }
    }
    /* Ownership. */
    for (int i = 0; i < npend; i++) {
        struct pending *p = &pend[i];
        char err[128];
        struct member m;
        int r;
        if (p->skip)
            continue;
        while ((r = archive_next(&p->ar, &m, err, sizeof err)) > 0) {
            const char *rel;
            char owner[PKG_NAME_MAX];
            if (member_rel(&m, &rel) < 0 || !rel || m.dir)
                continue;
            if (db_owner(rel, owner, sizeof owner) && strcmp(owner, p->m.name) != 0)
                return error(p->m.name, "%s belongs to %s", rel, owner);
            for (int j = 0; j < i; j++) {
                struct member other;
                if (!pend[j].skip && find_member(&pend[j], rel, &other))
                    return error(p->m.name, "%s is in %s as well", rel, pend[j].m.name);
            }
        }
        archive_rewind(&p->ar);
    }
    return 0;
}

/* ---- installation ---- */

static int remove_files(const struct record *r, const struct record *keep)
{
    char path[PKG_PATH_MAX];
    for (int i = 0; i < r->nfiles; i++) {
        int kept = 0;
        for (int j = 0; keep && j < keep->nfiles; j++)
            if (strcmp(keep->files[j].path, r->files[i].path) == 0)
                kept = 1;
        if (kept)
            continue;
        path_join(path, sizeof path, prefix, r->files[i].path);
        unlink(path);
    }
    for (int i = r->ndirs - 1; i >= 0; i--) {
        if (keep && record_has_dir(keep, r->dirs[i]))
            continue;
        path_join(path, sizeof path, prefix, r->dirs[i]);
        rmdir(path);
    }
    return 0;
}

/* Creates the directories of a prefix relative path, recording the new ones. */
static int make_parents(struct record *r, const char *rel, int including_last)
{
    char buf[PKG_PATH_MAX], full[PKG_PATH_MAX];
    strlcpy(buf, rel, sizeof buf);
    for (char *p = buf; *p; p++) {
        char *slash = strchr(p, '/');
        if (!slash && !including_last)
            break;
        if (slash)
            *slash = '\0';
        path_join(full, sizeof full, prefix, buf);
        struct stat st;
        if (stat(full, &st) < 0) {
            if (mkdir(full, 0755) < 0)
                return -1;
            record_add_dir(r, buf);
        } else if (!S_ISDIR(st.st_mode)) {
            errno = ENOTDIR;
            return -1;
        }
        if (!slash)
            break;
        *slash = '/';
        p = slash;
    }
    return 0;
}

static int extract(struct pending *p)
{
    struct record rec = {0}, old = {0};
    int have_old = p->upgrade && db_read_record(p->m.name, &old) == 0;
    if (have_old)
        for (int i = 0; i < old.ndirs; i++)
            record_add_dir(&rec, old.dirs[i]);
    char err[128], full[PKG_PATH_MAX];
    struct member m;
    int r, failed = 0;
    while (!failed && (r = archive_next(&p->ar, &m, err, sizeof err)) > 0) {
        const char *rel;
        if (member_rel(&m, &rel) < 0 || !rel)
            continue;
        if (m.dir) {
            if (make_parents(&rec, rel, 1) < 0)
                failed = 1;
            continue;
        }
        if (make_parents(&rec, rel, 0) < 0) {
            failed = 1;
            break;
        }
        path_join(full, sizeof full, prefix, rel);
        /* MiniOS chmod is currently a stub. Recreate payload files so the
         * archive's mode is applied by open, including during upgrades. */
        if (unlink(full) < 0 && errno != ENOENT) {
            error(p->m.name, "%s: %s", rel, strerror(errno));
            failed = 1;
            break;
        }
        if (write_file_mode(full, m.data, m.size, m.mode & 0777) < 0) {
            error(p->m.name, "%s: %s", rel, strerror(errno));
            failed = 1;
            break;
        }
        struct timespec ts[2] = { { m.mtime, 0 }, { m.mtime, 0 } };
        utimensat(AT_FDCWD, full, ts, 0);
        record_add_file(&rec, rel, m.size, gzip_crc32(m.data, m.size));
    }
    archive_rewind(&p->ar);
    if (!failed && db_write(p->m.name, &p->m, &rec) < 0) {
        error(p->m.name, "cannot write the record: %s", strerror(errno));
        failed = 1;
    }
    if (failed) {
        remove_files(&rec, have_old ? &old : NULL);
        if (!have_old)
            db_delete(p->m.name);
    } else if (have_old) {
        remove_files(&old, &rec);
    }
    if (have_old)
        record_free(&old);
    record_free(&rec);
    return failed ? -1 : 0;
}

/* Pending packages in an order that installs dependencies first. */
static int order(struct pending **out)
{
    int n = 0;
    for (int round = 0; round < npend; round++) {
        int progress = 0;
        for (int i = 0; i < npend; i++) {
            struct pending *p = &pend[i];
            if (p->done)
                continue;
            int ready = 1;
            for (int j = 0; j < p->m.ndeps; j++) {
                struct pending *q = pending_named(p->m.deps[j].name);
                if (q && q != p && !q->done)
                    ready = 0;
            }
            if (ready) {
                p->done = 1;
                out[n++] = p;
                progress = 1;
            }
        }
        if (!progress)
            break;
    }
    if (n < npend) {
        for (int i = 0; i < npend; i++)
            if (!pend[i].done)
                return error(pend[i].m.name, "dependency cycle");
    }
    return n;
}

/* The functions below install packages from repositories. */

static const struct index_entry *wanted[MAX_PENDING];
static int nwanted;
static char fetched_files[MAX_PENDING][PKG_PATH_MAX];
static char fetch_dir[PKG_PATH_MAX];

static const struct index_entry *wanted_named(const char *name)
{
    for (int i = 0; i < nwanted; i++)
        if (strcmp(wanted[i]->m.name, name) == 0)
            return wanted[i];
    return NULL;
}

static int add_wanted(const struct index_entry *e)
{
    const struct index_entry *w = wanted_named(e->m.name);
    if (w && w != e)
        return error(e->m.name, "wanted both as %s and as %s", w->m.version, e->m.version);
    if (w)
        return 0;
    if (nwanted == MAX_PENDING)
        return error(NULL, "at most %d packages per command", MAX_PENDING);
    wanted[nwanted++] = e;
    return 0;
}

/* An argument names a local archive when it is an existing file, has a
 * slash or ends in .mpk; anything else is a package name. */
static int is_file_argument(const char *arg)
{
    struct stat st;
    size_t n = strlen(arg);
    return stat(arg, &st) == 0 || strchr(arg, '/') || (n > 4 && strcmp(arg + n - 4, ".mpk") == 0);
}

/* resolve_name looks NAME or NAME-VERSION up in the index. It tries the
 * name as a whole first, since names may contain dashes, and then splits
 * it at the last dash. */
static const struct index_entry *resolve_name(const struct index *ix, const char *arg)
{
    const struct index_entry *e = index_best(ix, arg, NULL);
    if (e)
        return e;
    const char *dash = strrchr(arg, '-');
    if (dash && dash > arg && version_valid(dash + 1)) {
        char name[PKG_NAME_MAX];
        size_t n = (size_t)(dash - arg);
        if (n < sizeof name) {
            memcpy(name, arg, n);
            name[n] = '\0';
            if ((e = index_find(ix, name, dash + 1)) != NULL)
                return e;
            if (index_best(ix, name, NULL)) {
                error(name, "no repository offers version %s", dash + 1);
                return NULL;
            }
        }
    }
    error(arg, "no repository offers this package");
    return NULL;
}

static int system_library(const char *soname)
{
    char path[PKG_PATH_MAX];
    struct stat st;
    snprintf(path, sizeof path, "%s/lib/%s", sysroot, soname);
    return stat(path, &st) == 0;
}

/* resolve_dependencies adds what the wanted packages need and nothing
 * else provides. A
 * `depends` line that neither an installed package nor an archive on the
 * command line satisfies takes the highest version the index offers that
 * satisfies it. A `needs` soname that neither /lib, an installed package,
 * an archive on the command line nor another wanted package provides
 * takes a package the index lists as providing it with the same ABI
 * number. What the index cannot supply is left for the checks of install
 * to report. The list grows while it is walked. */
static int resolve_dependencies(const struct index *ix)
{
    for (int i = 0; i < nwanted; i++) {
        const struct manifest *m = &wanted[i]->m;
        for (int j = 0; j < m->ndeps; j++) {
            const struct pkg_dep *d = &m->deps[j];
            const struct manifest *im = installed_manifest(d->name);
            if (wanted_named(d->name) || pending_named(d->name) || (im && dep_satisfied(d, im->version)))
                continue;
            const struct index_entry *e = index_best(ix, d->name, d);
            if (e && add_wanted(e) < 0)
                return -1;
        }
        for (int j = 0; j < m->nneeds; j++) {
            const struct pkg_lib *n = &m->needs[j];
            int provided = system_library(n->soname);
            for (int k = 0; k < nwanted && !provided; k++)
                if (manifest_provides(&wanted[k]->m, n->soname))
                    provided = 1;
            for (int k = 0; k < npend && !provided; k++)
                if (manifest_provides(&pend[k].m, n->soname))
                    provided = 1;
            for (int k = 0; k < ninstalled && !provided; k++)
                if (manifest_provides(&installed_m[k], n->soname))
                    provided = 1;
            if (provided)
                continue;
            const struct index_entry *e = index_provider(ix, n);
            if (e && add_wanted(e) < 0)
                return -1;
        }
    }
    return 0;
}

/* fetch_wanted downloads every wanted package into a private directory
 * under /tmp and adds the verified archives to the pending list. */
static int fetch_wanted(const struct repo_config *c)
{
    snprintf(fetch_dir, sizeof fetch_dir, "/tmp/pkg-%d", (int)getpid());
    if (mkdir(fetch_dir, 0700) < 0 && errno != EEXIST)
        return error(NULL, "%s: %s", fetch_dir, strerror(errno));
    for (int i = 0; i < nwanted; i++) {
        const struct index_entry *e = wanted[i];
        if (npend == MAX_PENDING)
            return error(NULL, "at most %d packages per command", MAX_PENDING);
        char *file = fetched_files[npend];
        snprintf(file, PKG_PATH_MAX, "%s/%s-%s.mpk", fetch_dir, e->m.name, e->m.version);
        if (repo_fetch(c, e, file) < 0)
            return -1;
        printf("fetched %s %s from %s\n", e->m.name, e->m.version, c->repos[e->repo].name);
        struct pending *p = &pend[npend++];
        p->file = file;
        p->fetched = 1;
        if (open_package(p) < 0)
            return -1;
        if (strcmp(p->m.name, e->m.name) != 0 || strcmp(p->m.version, e->m.version) != 0)
            return error(e->m.name, "the archive holds %s %s, the index lists %s %s", p->m.name,
                         p->m.version, e->m.name, e->m.version);
    }
    return 0;
}

static void remove_fetched(void)
{
    for (int i = 0; i < npend; i++)
        if (pend[i].fetched)
            unlink(pend[i].file);
    if (fetch_dir[0])
        rmdir(fetch_dir);
}

/* install_from_repositories resolves the package names of an install,
 * check or upgrade command through the verified indexes, completes them
 * with their dependencies and fetches them. A wanted version that is
 * installed already is reported and not fetched. */
static int install_from_repositories(char **names, int nnames, const struct index_entry **upgrades, int nupgrades)
{
    static struct repo_config c;
    static struct index ix;
    if (config_read(&c) < 0)
        return -1;
    if (index_load(&c, &ix, 0) == 0)
        return error(NULL, "no repository index is available; run pkg update");
    for (int i = 0; i < nnames; i++) {
        const struct index_entry *e = resolve_name(&ix, names[i]);
        if (!e)
            return -1;
        const struct manifest *im = installed_manifest(e->m.name);
        if (im && strcmp(im->version, e->m.version) == 0 && !installed_foreign(e->m.name, im)) {
            printf("%s %s is installed already\n", e->m.name, e->m.version);
            continue;
        }
        if (add_wanted(e) < 0)
            return -1;
    }
    for (int i = 0; i < nupgrades; i++) {
        /* The upgrades were chosen from another load of the index. */
        const struct index_entry *e = index_find(&ix, upgrades[i]->m.name, upgrades[i]->m.version);
        if (e && add_wanted(e) < 0)
            return -1;
    }
    if (resolve_dependencies(&ix) < 0)
        return -1;
    return fetch_wanted(&c);
}

/* check_local_against_index compares every local archive with the
 * indexes. Its size and digest must be the ones the index gives for its
 * name and version. */
static int check_local_against_index(void)
{
    struct repo_config c;
    struct index ix;
    if (!config_present() || config_read(&c) < 0)
        return 0;
    index_load(&c, &ix, 1);
    int status = 0;
    for (int i = 0; i < npend; i++) {
        struct pending *p = &pend[i];
        if (p->fetched)
            continue;
        const struct index_entry *e = index_find(&ix, p->m.name, p->m.version);
        if (!e) {
            printf("%s %s is in no repository index\n", p->m.name, p->m.version);
            continue;
        }
        char err[400];
        int m = file_matches(e, p->file, p->file, c.repos[e->repo].name, err, sizeof err);
        if (m == 1)
            printf("%s %s matches the index of %s\n", p->m.name, p->m.version, c.repos[e->repo].name);
        else
            status = error(p->m.name, "%s", err);
    }
    index_free(&ix);
    return status;
}

static int install(int argc, char **argv, const struct index_entry **upgrades, int nupgrades, int check_only)
{
    char *names[MAX_PENDING];
    int nnames = 0;
    if (argc > MAX_PENDING)
        return error(NULL, "at most %d packages per command", MAX_PENDING);
    int r = db_lock();
    if (r < 0)
        return error(NULL, "cannot lock %s/lib/pkg: %s", prefix, strerror(-r));
    load_installed();
    int status = 0;
    for (int i = 0; i < argc && status == 0; i++) {
        if (!is_file_argument(argv[i])) {
            names[nnames++] = argv[i];
            continue;
        }
        pend[npend].file = argv[i];
        if (open_package(&pend[npend]) < 0)
            status = -1;
        npend++;
    }
    if (status == 0 && (nnames || nupgrades))
        status = install_from_repositories(names, nnames, upgrades, nupgrades);
    if (status == 0 && npend == 0) {
        db_unlock();
        remove_fetched();
        return 0;
    }
    if (status == 0)
        status = check_all();
    struct pending *sorted[MAX_PENDING];
    if (status == 0 && order(sorted) < 0)
        status = -1;
    if (status == 0 && check_only) {
        for (int i = 0; i < npend; i++)
            if (!pend[i].skip)
                printf("%s %s can be installed\n", pend[i].m.name, pend[i].m.version);
        status = check_local_against_index();
    } else if (status == 0) {
        for (int i = 0; i < npend && status == 0; i++) {
            struct pending *p = sorted[i];
            if (p->skip)
                continue;
            if (extract(p) < 0) {
                status = -1;
                break;
            }
            printf("%s %s %s\n", p->upgrade ? "upgraded" : "installed", p->m.name, p->m.version);
        }
        r = db_write_tables();
        if (r < 0)
            status = error(NULL, "cannot write the launcher and MIME tables: %s", strerror(-r));
    }
    for (int i = 0; i < npend; i++)
        archive_free(&pend[i].ar);
    remove_fetched();
    db_unlock();
    return status;
}

static int cmd_install(int argc, char **argv, int check_only)
{
    if (argc < 1)
        usage();
    return install(argc, argv, NULL, 0, check_only);
}

/* cmd_upgrade installs the higher versions that the indexes offer for the
 * installed packages, all of them or the named ones. */
static int cmd_upgrade(int argc, char **argv)
{
    struct repo_config c;
    struct index ix;
    const struct index_entry *newer[MAX_PENDING];
    int n = 0, status = 0;
    if (config_read(&c) < 0)
        return -1;
    if (index_load(&c, &ix, 0) == 0)
        return error(NULL, "no repository index is available; run pkg update");
    load_installed();
    for (int i = 0; i < argc; i++)
        if (!installed_manifest(argv[i]))
            status = error(argv[i], "not installed");
    for (int i = 0; i < ninstalled && status == 0; i++) {
        const struct manifest *im = &installed_m[i];
        int named = argc == 0;
        for (int j = 0; j < argc; j++)
            if (strcmp(argv[j], installed[i]) == 0)
                named = 1;
        if (!im->name[0] || !named)
            continue;
        const struct index_entry *e = index_best(&ix, im->name, NULL);
        if (!e || version_cmp(e->m.version, im->version) <= 0)
            continue;
        if (n == MAX_PENDING)
            status = error(NULL, "at most %d packages per command", MAX_PENDING);
        else
            newer[n++] = e;
    }
    if (status == 0 && n == 0)
        printf("the installed packages are up to date\n");
    else if (status == 0)
        status = install(0, NULL, newer, n, 0);
    index_free(&ix);
    return status;
}

/* ---- remove ---- */

static int cmd_remove(int argc, char **argv)
{
    if (argc >= 1 && strcmp(argv[0], "--force") == 0) {
        force = 1;
        argc--;
        argv++;
    }
    if (argc < 1)
        usage();
    int r = db_lock();
    if (r < 0)
        return error(NULL, "cannot lock %s/lib/pkg: %s", prefix, strerror(-r));
    load_installed();
    int status = 0;
    for (int i = 0; i < argc && status == 0; i++) {
        const char *name = argv[i];
        const struct manifest *m = installed_manifest(name);
        if (!m) {
            status = error(name, "not installed");
            break;
        }
        for (int j = 0; j < ninstalled && !force; j++) {
            int listed = 0;
            for (int k = 0; k < argc; k++)
                if (strcmp(argv[k], installed[j]) == 0)
                    listed = 1;
            if (listed)
                continue;
            for (int k = 0; k < installed_m[j].ndeps; k++)
                if (strcmp(installed_m[j].deps[k].name, name) == 0)
                    status = error(name, "%s depends on it (--force removes it anyway)", installed[j]);
            for (int k = 0; k < installed_m[j].nneeds && status == 0; k++)
                if (manifest_provides(m, installed_m[j].needs[k].soname))
                    status = error(name, "%s needs %s from it (--force removes it anyway)", installed[j], installed_m[j].needs[k].soname);
        }
        if (status < 0)
            break;
        struct record rec;
        if (db_read_record(name, &rec) == 0) {
            remove_files(&rec, NULL);
            record_free(&rec);
        }
        r = db_delete(name);
        if (r < 0)
            status = error(name, "cannot remove the record: %s", strerror(-r));
        else
            printf("removed %s %s\n", name, m->version);
    }
    r = db_write_tables();
    if (r < 0)
        status = error(NULL, "cannot write the launcher and MIME tables: %s", strerror(-r));
    db_unlock();
    return status;
}

/* ---- list, info, verify ---- */

static int cmd_list(void)
{
    load_installed();
    for (int i = 0; i < ninstalled; i++)
        if (installed_m[i].name[0])
            printf("%s %s %s\n", installed[i], installed_m[i].version, installed_m[i].summary);
    return 0;
}

static int cmd_info(const char *arg)
{
    struct manifest m;
    struct stat st;
    if (stat(arg, &st) == 0 && S_ISREG(st.st_mode)) {
        struct pending p = { .file = arg };
        if (open_package(&p) < 0)
            return -1;
        manifest_write(stdout, &p.m);
        char err[128];
        struct member mem;
        while (archive_next(&p.ar, &mem, err, sizeof err) > 0) {
            const char *rel;
            if (member_rel(&mem, &rel) == 0 && rel && !mem.dir)
                printf("file %s %zu\n", rel, mem.size);
        }
        archive_free(&p.ar);
        return 0;
    }
    if (db_read(arg, &m) < 0)
        return error(arg, "not installed");
    manifest_write(stdout, &m);
    struct record rec;
    if (db_read_record(arg, &rec) == 0) {
        for (int i = 0; i < rec.nfiles; i++)
            printf("file %s %zu\n", rec.files[i].path, rec.files[i].size);
        record_free(&rec);
    }
    return 0;
}

static int verify_one(const char *name)
{
    struct record rec;
    if (db_read_record(name, &rec) < 0)
        return error(name, "not installed");
    int bad = 0;
    for (int i = 0; i < rec.nfiles; i++) {
        char path[PKG_PATH_MAX];
        uint8_t *data;
        size_t len;
        path_join(path, sizeof path, prefix, rec.files[i].path);
        if (read_file(path, &data, &len) < 0) {
            printf("%s: %s: missing\n", name, rec.files[i].path);
            bad = 1;
            continue;
        }
        if (len != rec.files[i].size || gzip_crc32(data, len) != rec.files[i].crc) {
            printf("%s: %s: changed\n", name, rec.files[i].path);
            bad = 1;
        }
        free(data);
    }
    record_free(&rec);
    return bad ? -1 : 0;
}

static int cmd_verify(int argc, char **argv)
{
    int status = 0;
    if (argc == 0) {
        load_installed();
        for (int i = 0; i < ninstalled; i++)
            if (verify_one(installed[i]) < 0)
                status = -1;
    } else {
        for (int i = 0; i < argc; i++)
            if (verify_one(argv[i]) < 0)
                status = -1;
    }
    return status;
}

/* ---- build ---- */

struct build {
    struct tar_writer w;
    struct manifest *m;
    struct manifest given;      /* the needs lines of the source manifest */
    char root[PKG_PATH_MAX];
};

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static int add_needed(struct build *b, const uint8_t *data, size_t len, const char *rel)
{
    char (*names)[PKG_NAME_MAX] = calloc(MAX_LIBS, PKG_NAME_MAX);
    int n = elf_needed(data, len, names, MAX_LIBS);
    for (int i = 0; i < n && i < MAX_LIBS; i++) {
        int seen = 0;
        for (int j = 0; j < b->m->nneeds; j++)
            if (strcmp(b->m->needs[j].soname, names[i]) == 0)
                seen = 1;
        if (seen)
            continue;
        int abi;
        const struct pkg_lib *own = manifest_provides(b->m, names[i]);
        if (own) {
            abi = own->abi;
        } else {
            struct lib *l = find_library(names[i]);
            abi = l ? l->abi : -1;
        }
        for (int j = 0; j < b->given.nneeds && abi < 0; j++)
            if (strcmp(b->given.needs[j].soname, names[i]) == 0)
                abi = b->given.needs[j].abi;
        if (abi < 0) {
            error(b->m->name, "%s needs %s, whose ABI number is unknown", rel, names[i]);
            free(names);
            return -1;
        }
        if (b->m->nneeds >= PKG_MAX_LIBS) {
            free(names);
            return error(b->m->name, "too many libraries");
        }
        strlcpy(b->m->needs[b->m->nneeds].soname, names[i], PKG_NAME_MAX);
        b->m->needs[b->m->nneeds].abi = abi;
        b->m->nneeds++;
    }
    free(names);
    return 0;
}

/* The arch line of the manifest is the machine of the ELF files, which
 * must all be built for the same machine. */
static int note_arch(struct build *b, const uint8_t *data, size_t len, const char *rel)
{
    const char *arch = elf_arch(data, len);
    if (!b->m->arch[0]) {
        strlcpy(b->m->arch, arch, sizeof b->m->arch);
        return 0;
    }
    if (strcmp(b->m->arch, arch) != 0)
        return error(b->m->name, "%s is built for %s, and the package for %s", rel, arch, b->m->arch);
    return 0;
}

static int build_dir(struct build *b, const char *rel)
{
    char path[PKG_PATH_MAX];
    if (rel[0])
        path_join(path, sizeof path, b->root, rel);
    else
        strlcpy(path, b->root, sizeof path);
    DIR *d = opendir(path);
    if (!d)
        return error(b->m->name, "%s: %s", path, strerror(errno));
    char **names = NULL;
    int n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            names = realloc(names, (size_t)cap * sizeof *names);
        }
        names[n++] = strdup(e->d_name);
    }
    closedir(d);
    if (n)
        qsort(names, (size_t)n, sizeof *names, cmp_str);
    int r = 0;
    for (int i = 0; i < n && r == 0; i++) {
        char sub[PKG_PATH_MAX], full[PKG_PATH_MAX], member[PKG_PATH_MAX];
        if (rel[0])
            snprintf(sub, sizeof sub, "%s/%s", rel, names[i]);
        else
            strlcpy(sub, names[i], sizeof sub);
        path_join(full, sizeof full, b->root, sub);
        snprintf(member, sizeof member, "files/%s", sub);
        struct stat st;
        if (lstat(full, &st) < 0) {
            r = error(b->m->name, "%s: %s", full, strerror(errno));
        } else if (S_ISLNK(st.st_mode)) {
            /* The package format has no link member. */
            r = error(b->m->name, "%s: symbolic links cannot be packaged", full);
        } else if (S_ISDIR(st.st_mode)) {
            if (tarw_add(&b->w, member, 1, 0755, st.st_mtime, NULL, 0) < 0)
                r = error(b->m->name, "%s: cannot add", member);
            else
                r = build_dir(b, sub);
        } else if (S_ISREG(st.st_mode)) {
            uint8_t *data;
            size_t len;
            if (read_file(full, &data, &len) < 0) {
                r = error(b->m->name, "%s: %s", full, strerror(errno));
            } else {
                if (elf_is(data, len))
                    r = note_arch(b, data, len, sub);
                if (r == 0 && elf_is(data, len))
                    r = add_needed(b, data, len, sub);
                if (r == 0 && tarw_add(&b->w, member, 0, st.st_mode & 0777, st.st_mtime, data, len) < 0)
                    r = error(b->m->name, "%s: cannot add", member);
                free(data);
            }
        } else {
            r = error(b->m->name, "%s: not a regular file or a directory", full);
        }
    }
    for (int i = 0; i < n; i++)
        free(names[i]);
    free(names);
    return r;
}

static int cmd_build(int argc, char **argv)
{
    if (argc < 1 || argc > 2)
        usage();
    char path[PKG_PATH_MAX], err[256];
    struct manifest m;
    path_join(path, sizeof path, argv[0], "manifest");
    if (manifest_read(&m, path, err, sizeof err) < 0)
        return error(NULL, "%s", err);
    load_installed();
    struct build b = { .m = &m };
    /* The needs lines are derived from the ELF files; a line of the
     * source manifest supplies the number of a library that is neither
     * on this system nor installed. */
    b.given = m;
    m.nneeds = 0;
    path_join(b.root, sizeof b.root, argv[0], "files");
    struct tar_writer files = {0};
    b.w = files;
    /* The tree first, which derives the needs lines, then the manifest is
     * placed before it. */
    if (build_dir(&b, "") < 0)
        return -1;
    struct tar_writer w = {0};
    char tmp[PKG_PATH_MAX];
    snprintf(tmp, sizeof tmp, "/tmp/pkg-manifest-%d", (int)getpid());
    FILE *f = fopen(tmp, "w");
    if (!f)
        return error(m.name, "%s: %s", tmp, strerror(errno));
    manifest_write(f, &m);
    fclose(f);
    uint8_t *text;
    size_t textlen;
    if (read_file(tmp, &text, &textlen) < 0)
        return error(m.name, "%s: %s", tmp, strerror(errno));
    unlink(tmp);
    if (tarw_add(&w, "manifest", 0, 0644, time(NULL), text, textlen) < 0)
        return error(m.name, "cannot write the manifest");
    free(text);
    if (tarw_add(&w, "files", 1, 0755, time(NULL), NULL, 0) < 0)
        return error(m.name, "cannot write the archive");
    size_t need = w.len + b.w.len;
    uint8_t *all = realloc(w.data, need + 1024);
    if (!all)
        return error(m.name, "out of memory");
    memcpy(all + w.len, b.w.data, b.w.len);
    w.data = all;
    w.cap = need + 1024;
    w.len = need;
    free(b.w.data);
    char out[PKG_PATH_MAX];
    if (argc == 2)
        strlcpy(out, argv[1], sizeof out);
    else
        snprintf(out, sizeof out, "%s-%s.mpk", m.name, m.version);
    if (tarw_finish(&w, out) < 0)
        return error(m.name, "%s: %s", out, strerror(errno));
    printf("%s\n", out);
    return 0;
}

int main(int argc, char **argv)
{
    int i = 1;
    while (argc > i + 1 && (strcmp(argv[i], "--prefix") == 0 || strcmp(argv[i], "--root") == 0 ||
                            strcmp(argv[i], "--config") == 0)) {
        if (argv[i][2] == 'p')
            prefix = argv[i + 1];
        else if (argv[i][2] == 'r')
            sysroot = argv[i + 1];
        else
            config_path = argv[i + 1];
        i += 2;
    }
    if (i >= argc)
        usage();
    const char *cmd = argv[i++];
    int r = 0;
    if (strcmp(cmd, "install") == 0)
        r = cmd_install(argc - i, argv + i, 0);
    else if (strcmp(cmd, "check") == 0)
        r = cmd_install(argc - i, argv + i, 1);
    else if (strcmp(cmd, "update") == 0 && argc == i)
        r = cmd_update();
    else if (strcmp(cmd, "search") == 0)
        r = cmd_search(argc - i, argv + i);
    else if (strcmp(cmd, "upgrade") == 0)
        r = cmd_upgrade(argc - i, argv + i);
    else if (strcmp(cmd, "remove") == 0)
        r = cmd_remove(argc - i, argv + i);
    else if (strcmp(cmd, "list") == 0)
        r = cmd_list();
    else if (strcmp(cmd, "info") == 0 && argc - i == 1)
        r = cmd_info(argv[i]);
    else if (strcmp(cmd, "verify") == 0)
        r = cmd_verify(argc - i, argv + i);
    else if (strcmp(cmd, "build") == 0)
        r = cmd_build(argc - i, argv + i);
    else
        usage();
    return r < 0 ? 1 : 0;
}
