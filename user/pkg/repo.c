/* This file implements signed repositories (docs/design/packages.md). A
 * repository is a directory served over HTTP holding the archives, an index that lists
 * each archive with its name, version, size and SHA-256 digest, and a
 * detached Ed25519 signature of the index. The transport is plain HTTP,
 * so nothing fetched is trusted until the signature of the index verifies
 * against a key under /etc/pkg/keys and an archive matches the size and
 * digest the index gives for it.
 *
 * `pkg update` fetches the index and its signature into
 * <prefix>/lib/pkg/_repos/<name>/, verifies both and only then replaces
 * the previous copy, so a refused index leaves the last verified one in
 * place. Every later reader verifies the cached copy again. The
 * underscore keeps the directory apart from the package records, whose
 * names cannot contain one. */
#include "pkg.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <minios/ed25519.h>
#include <minios/http.h>
#include <minios/sha2.h>

/* An index may hold 4 MiB and a signature file 1 KiB, and the indexes of
 * all repositories together may list 512 entries. */
#define INDEX_MAX (4 << 20)
#define SIG_MAX 1024
#define INDEX_ENTRIES_MAX 512
#define INDEX_MAGIC "minios-pkg-index 1"

/* --config sets config_path; without it pkg reads <root>/etc/pkg.conf. */
const char *config_path;

static int report(const char *who, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static int report(const char *who, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (who)
        fprintf(stderr, "pkg: %s: ", who);
    else
        fputs("pkg: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    return -1;
}

static int unhex(uint8_t *out, size_t n, const char *hex)
{
    if (strlen(hex) != 2 * n)
        return -1;
    for (size_t i = 0; i < n; i++) {
        int v = 0;
        for (int k = 0; k < 2; k++) {
            char c = hex[2 * i + k];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else return -1;
        }
        out[i] = (uint8_t)v;
    }
    return 0;
}

static void tohex(char *out, const uint8_t *data, size_t n)
{
    for (size_t i = 0; i < n; i++)
        snprintf(out + 2 * i, 3, "%02x", data[i]);
}

/* next_line splits the next line off text. It returns the line without
 * its newline and trimmed of blanks at both ends, or NULL at the end. */
static char *next_line(char **text)
{
    char *line = *text;
    if (!line || !*line)
        return NULL;
    char *nl = strchr(line, '\n');
    if (nl) {
        *nl = '\0';
        *text = nl + 1;
    } else {
        *text = line + strlen(line);
    }
    while (*line == ' ' || *line == '\t')
        line++;
    char *end = line + strlen(line);
    while (end > line && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
        *--end = '\0';
    return line;
}

static void config_file(char *path, size_t n)
{
    if (config_path)
        strlcpy(path, config_path, n);
    else
        snprintf(path, n, "%s/etc/pkg.conf", sysroot);
}

int config_present(void)
{
    char path[PKG_PATH_MAX];
    struct stat st;
    config_file(path, sizeof path);
    return stat(path, &st) == 0;
}

/* /etc/pkg.conf holds `repo NAME URL` lines, searched in their order,
 * and an optional `timeout SECONDS`. */
int config_read(struct repo_config *c)
{
    char path[PKG_PATH_MAX];
    config_file(path, sizeof path);
    memset(c, 0, sizeof *c);
    c->timeout = 30;
    uint8_t *data;
    size_t len;
    if (read_file(path, &data, &len) < 0)
        return report(NULL, "%s: %s", path, strerror(errno));
    char *text = (char *)data, *line;
    int n = 0, r = 0;
    while (r == 0 && (line = next_line(&text)) != NULL) {
        n++;
        if (!*line || *line == '#')
            continue;
        char key[16], a[PKG_URL_MAX], b[PKG_URL_MAX], extra[2];
        int fields = sscanf(line, "%15s %511s %511s %1s", key, a, b, extra);
        if (strcmp(key, "repo") == 0 && fields == 3) {
            struct http_url u;
            size_t ul = strlen(b);
            while (ul > 7 && b[ul - 1] == '/')
                b[--ul] = '\0';
            if (!name_valid(a))
                r = report(NULL, "%s line %d: invalid repository name %s", path, n, a);
            else if (http_parse_url(b, &u) < 0)
                r = report(NULL, "%s line %d: %s is not an http:// URL", path, n, b);
            else if (c->nrepos == PKG_MAX_REPOS)
                r = report(NULL, "%s line %d: more than %d repositories", path, n, PKG_MAX_REPOS);
            for (int i = 0; r == 0 && i < c->nrepos; i++)
                if (strcmp(c->repos[i].name, a) == 0)
                    r = report(NULL, "%s line %d: repository %s named twice", path, n, a);
            if (r == 0) {
                strlcpy(c->repos[c->nrepos].name, a, PKG_NAME_MAX);
                strlcpy(c->repos[c->nrepos].url, b, PKG_URL_MAX);
                c->nrepos++;
            }
        } else if (strcmp(key, "timeout") == 0 && fields == 2 && atoi(a) > 0) {
            c->timeout = atoi(a);
        } else {
            r = report(NULL, "%s line %d: expected repo NAME URL or timeout SECONDS", path, n);
        }
    }
    free(data);
    if (r == 0 && c->nrepos == 0)
        r = report(NULL, "%s names no repository", path);
    return r;
}

/* A signature file names its key by the first eight bytes of the SHA-256
 * of the public key, in hex. */
static void key_id(char id[17], const uint8_t pub[ED25519_PUBLIC_SIZE])
{
    uint8_t d[SHA256_DIGEST_SIZE];
    sha256(pub, ED25519_PUBLIC_SIZE, d);
    tohex(id, d, 8);
}

/* key_lookup finds the trusted key named id. It returns 0 with pub and
 * the file name set, or -1 when no key file under <root>/etc/pkg/keys has
 * that id. A key file holds one line, `ed25519 HEX`. */
static int key_lookup(const char *id, uint8_t pub[ED25519_PUBLIC_SIZE], char *file, size_t filelen)
{
    char dir[PKG_PATH_MAX];
    snprintf(dir, sizeof dir, "%s/etc/pkg/keys", sysroot);
    DIR *d = opendir(dir);
    if (!d)
        return -1;
    struct dirent *e;
    int found = -1;
    while (found < 0 && (e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);
        if (n < 5 || strcmp(e->d_name + n - 4, ".pub") != 0)
            continue;
        char path[PKG_PATH_MAX], kind[16], hex[80];
        uint8_t *data;
        size_t len;
        path_join(path, sizeof path, dir, e->d_name);
        if (read_file(path, &data, &len) < 0)
            continue;
        char *text = (char *)data, *line;
        while ((line = next_line(&text)) != NULL) {
            char this_id[17];
            if (!*line || *line == '#')
                continue;
            if (sscanf(line, "%15s %79s", kind, hex) == 2 && strcmp(kind, "ed25519") == 0 &&
                unhex(pub, ED25519_PUBLIC_SIZE, hex) == 0) {
                key_id(this_id, pub);
                if (strcmp(this_id, id) == 0) {
                    snprintf(file, filelen, "%s", path);
                    found = 0;
                }
            }
            break;
        }
        free(data);
    }
    closedir(d);
    return found;
}

/* index_verify checks the signature file sig (`ed25519 KEYID HEX`) over
 * the index text. err receives the reason of a refusal. */
static int index_verify(const uint8_t *text, size_t len, const char *sig, char *err, size_t errlen)
{
    char kind[16], id[32], hex[160];
    uint8_t signature[ED25519_SIGNATURE_SIZE], pub[ED25519_PUBLIC_SIZE];
    char keyfile[PKG_PATH_MAX];
    if (sscanf(sig, "%15s %31s %159s", kind, id, hex) != 3 || strcmp(kind, "ed25519") != 0 ||
        strlen(id) != 16 || unhex(signature, sizeof signature, hex) < 0) {
        snprintf(err, errlen, "the signature file is malformed");
        return -1;
    }
    if (key_lookup(id, pub, keyfile, sizeof keyfile) < 0) {
        snprintf(err, errlen, "the index is signed by key %s, which is not in %s/etc/pkg/keys", id, sysroot);
        return -1;
    }
    if (!ed25519_verify(signature, text, len, pub)) {
        snprintf(err, errlen, "the index signature does not verify with %s", keyfile);
        return -1;
    }
    return 0;
}

/* An archive path is relative to the repository URL. It consists of
 * plain characters and has no empty, dot or dot-dot components. */
static int archive_path_valid(const char *p)
{
    if (!*p || *p == '/' || strlen(p) >= PKG_PATH_MAX)
        return 0;
    for (const char *s = p; *s; s++)
        if (!isalnum((unsigned char)*s) && !strchr("._+-/", *s))
            return 0;
    for (const char *s = p; *s; ) {
        const char *e = strchr(s, '/');
        size_t n = e ? (size_t)(e - s) : strlen(s);
        if (n == 0 || (n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.'))
            return 0;
        s += n;
        if (*s == '/') s++;
    }
    return 1;
}

/* entry_parse parses the entry whose lines are in text. The manifest keys
 * go through manifest_parse, and path, size and sha256 are the index's
 * own. */
static int entry_parse(struct index_entry *e, char *text, char *err, size_t errlen)
{
    static const char *const manifest_keys[] = { "name", "version", "summary", "depends", "conflicts", "provides", "needs" };
    char *manifest = malloc(strlen(text) + 1), *line;
    size_t mlen = 0;
    int have_size = 0, have_sha = 0, r = 0;
    if (!manifest) {
        snprintf(err, errlen, "out of memory");
        return -1;
    }
    memset(e, 0, sizeof *e);
    while (r == 0 && (line = next_line(&text)) != NULL) {
        if (!*line || *line == '#')
            continue;
        char *value = line + strcspn(line, " \t");
        size_t keylen = (size_t)(value - line);
        while (*value == ' ' || *value == '\t')
            value++;
        if (keylen == 4 && strncmp(line, "path", 4) == 0) {
            if (e->path[0] || !archive_path_valid(value))
                r = -1, snprintf(err, errlen, "invalid path %s", value);
            else
                strlcpy(e->path, value, sizeof e->path);
        } else if (keylen == 4 && strncmp(line, "size", 4) == 0) {
            char *end;
            e->size = strtoll(value, &end, 10);
            if (have_size++ || *end || e->size <= 0)
                r = -1, snprintf(err, errlen, "invalid size %s", value);
        } else if (keylen == 6 && strncmp(line, "sha256", 6) == 0) {
            if (have_sha++ || unhex(e->sha256, sizeof e->sha256, value) < 0)
                r = -1, snprintf(err, errlen, "invalid sha256 %s", value);
        } else {
            int known = 0;
            for (size_t k = 0; k < sizeof manifest_keys / sizeof manifest_keys[0]; k++)
                if (strlen(manifest_keys[k]) == keylen && strncmp(line, manifest_keys[k], keylen) == 0)
                    known = 1;
            if (!known) {
                r = -1;
                snprintf(err, errlen, "unknown key %.*s", (int)keylen, line);
            } else {
                size_t n = strlen(line);
                memcpy(manifest + mlen, line, n);
                mlen += n;
                manifest[mlen++] = '\n';
            }
        }
    }
    if (r == 0 && manifest_parse(&e->m, manifest, mlen, err, errlen) < 0)
        r = -1;
    if (r == 0 && (!e->path[0] || !have_size || !have_sha)) {
        snprintf(err, errlen, "%s %s lacks its path, size or sha256", e->m.name, e->m.version);
        r = -1;
    }
    free(manifest);
    return r;
}

/* index_parse adds the entries of an index text to ix. The first line
 * names the format, and every entry begins with a name line. */
static int index_parse(struct index *ix, int repo, char *text, char *err, size_t errlen)
{
    char *first = next_line(&text);
    if (!first || strcmp(first, INDEX_MAGIC) != 0) {
        snprintf(err, errlen, "the index does not begin with %s", INDEX_MAGIC);
        return -1;
    }
    while (*text) {
        /* The entry runs to the next line beginning with "name ". */
        char *start = text, *end = text;
        int seen_name = 0;
        for (;;) {
            char *line = end;
            while (*line == ' ' || *line == '\t')
                line++;
            if (!*end)
                break;
            if (strncmp(line, "name ", 5) == 0 || strncmp(line, "name\t", 5) == 0) {
                if (seen_name)
                    break;
                seen_name = 1;
            }
            char *nl = strchr(end, '\n');
            end = nl ? nl + 1 : end + strlen(end);
        }
        char saved = *end;
        *end = '\0';
        if (!seen_name) {
            /* Only blank lines and comments may follow the last entry. */
            char *line;
            while ((line = next_line(&start)) != NULL)
                if (*line && *line != '#') {
                    snprintf(err, errlen, "line outside an entry: %s", line);
                    return -1;
                }
            break;
        }
        if (ix->n == ix->cap) {
            if (ix->cap >= INDEX_ENTRIES_MAX) {
                snprintf(err, errlen, "more than %d entries", INDEX_ENTRIES_MAX);
                return -1;
            }
            int cap = ix->cap ? ix->cap * 2 : 16;
            struct index_entry *grown = realloc(ix->entries, (size_t)cap * sizeof *grown);
            if (!grown) {
                snprintf(err, errlen, "out of memory");
                return -1;
            }
            ix->entries = grown;
            ix->cap = cap;
        }
        struct index_entry *e = &ix->entries[ix->n];
        char entry_err[200];
        if (entry_parse(e, start, entry_err, sizeof entry_err) < 0) {
            snprintf(err, errlen, "entry %d: %s", ix->n + 1, entry_err);
            return -1;
        }
        e->repo = repo;
        for (int i = 0; i < ix->n; i++)
            if (ix->entries[i].repo == repo && strcmp(ix->entries[i].m.name, e->m.name) == 0 &&
                strcmp(ix->entries[i].m.version, e->m.version) == 0) {
                snprintf(err, errlen, "%s %s is listed twice", e->m.name, e->m.version);
                return -1;
            }
        ix->n++;
        *end = saved;
        text = end;
    }
    return 0;
}

void index_free(struct index *ix)
{
    free(ix->entries);
    memset(ix, 0, sizeof *ix);
}

static void repo_dir(char *buf, size_t n, const struct repo *r)
{
    snprintf(buf, n, "%s/lib/pkg/_repos/%s", prefix, r->name);
}

/* load_one reads, verifies and parses the cached index of repository i. */
static int load_one(const struct repo_config *c, int i, struct index *ix, char *err, size_t errlen)
{
    char dir[PKG_PATH_MAX], path[PKG_PATH_MAX];
    uint8_t *text = NULL, *sig = NULL, *url = NULL;
    size_t len, siglen, urllen;
    int r = -1;
    repo_dir(dir, sizeof dir, &c->repos[i]);
    path_join(path, sizeof path, dir, "url");
    if (read_file(path, &url, &urllen) < 0) {
        snprintf(err, errlen, "no index; run pkg update");
        goto out;
    }
    if (urllen && url[urllen - 1] == '\n')
        url[urllen - 1] = '\0';
    if (strcmp((char *)url, c->repos[i].url) != 0) {
        snprintf(err, errlen, "the index was fetched from %s; run pkg update", (char *)url);
        goto out;
    }
    path_join(path, sizeof path, dir, "index");
    if (read_file(path, &text, &len) < 0) {
        snprintf(err, errlen, "%s: %s", path, strerror(errno));
        goto out;
    }
    path_join(path, sizeof path, dir, "index.sig");
    if (read_file(path, &sig, &siglen) < 0) {
        snprintf(err, errlen, "%s: %s", path, strerror(errno));
        goto out;
    }
    if (index_verify(text, len, (char *)sig, err, errlen) < 0)
        goto out;
    if (memchr(text, '\0', len)) {
        snprintf(err, errlen, "the index contains a zero byte");
        goto out;
    }
    r = index_parse(ix, i, (char *)text, err, errlen);
out:
    free(text);
    free(sig);
    free(url);
    return r;
}

/* index_load collects the verified entries of every configured
 * repository. A repository without a usable index is left out and, unless
 * quiet is set, reported. It returns the number of repositories loaded. */
int index_load(const struct repo_config *c, struct index *ix, int quiet)
{
    memset(ix, 0, sizeof *ix);
    int loaded = 0;
    for (int i = 0; i < c->nrepos; i++) {
        char err[300];
        int before = ix->n;
        if (load_one(c, i, ix, err, sizeof err) < 0) {
            ix->n = before;
            if (!quiet)
                report(c->repos[i].name, "%s", err);
            continue;
        }
        loaded++;
    }
    return loaded;
}

/* index_best returns the highest version of name that satisfies want,
 * any version when want is NULL. Between equal versions the repository
 * named first wins. */
const struct index_entry *index_best(const struct index *ix, const char *name, const struct pkg_dep *want)
{
    const struct index_entry *best = NULL;
    for (int i = 0; i < ix->n; i++) {
        const struct index_entry *e = &ix->entries[i];
        if (strcmp(e->m.name, name) != 0 || (want && !dep_satisfied(want, e->m.version)))
            continue;
        if (!best || version_cmp(e->m.version, best->m.version) > 0)
            best = e;
    }
    return best;
}

const struct index_entry *index_find(const struct index *ix, const char *name, const char *version)
{
    for (int i = 0; i < ix->n; i++)
        if (strcmp(ix->entries[i].m.name, name) == 0 && version_cmp(ix->entries[i].m.version, version) == 0)
            return &ix->entries[i];
    return NULL;
}

/* index_provider returns the highest version of a package that provides
 * lib with its ABI number. */
const struct index_entry *index_provider(const struct index *ix, const struct pkg_lib *lib)
{
    const struct index_entry *best = NULL;
    for (int i = 0; i < ix->n; i++) {
        const struct pkg_lib *p = manifest_provides(&ix->entries[i].m, lib->soname);
        if (!p || p->abi != lib->abi)
            continue;
        if (!best || (strcmp(best->m.name, ix->entries[i].m.name) == 0 &&
                      version_cmp(ix->entries[i].m.version, best->m.version) > 0))
            best = &ix->entries[i];
    }
    return best;
}

/* fetch stores url in path and refuses more than max bytes and any
 * status but 200. err receives the reason of a failure. */
static int fetch(const char *url, const char *path, int timeout, long long max, char *err, size_t errlen)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        snprintf(err, errlen, "%s: %s", path, strerror(errno));
        return -1;
    }
    struct http_response res;
    int r = http_get(url, fd, timeout, max, &res);
    if (close(fd) < 0 && r == 0) {
        snprintf(err, errlen, "%s: %s", path, strerror(errno));
        r = -1;
    } else if (r < 0) {
        snprintf(err, errlen, "%s", res.error);
    } else if (res.status != 200) {
        snprintf(err, errlen, "%s: the server returned status %d", url, res.status);
        r = -1;
    }
    if (r < 0)
        unlink(path);
    return r < 0 ? -1 : 0;
}

/* file_matches returns 1 when the file has the size and digest the entry
 * gives, 0 when it differs, and -1 when it cannot be read. err says how
 * the file differs, naming it as label and the index by repo. */
int file_matches(const struct index_entry *e, const char *file, const char *label, const char *repo,
                 char *err, size_t errlen)
{
    uint8_t *data, digest[SHA256_DIGEST_SIZE];
    size_t len;
    if (read_file(file, &data, &len) < 0) {
        snprintf(err, errlen, "%s: %s", file, strerror(errno));
        return -1;
    }
    sha256(data, len, digest);
    free(data);
    if ((long long)len != e->size) {
        snprintf(err, errlen, "%s is %zu bytes, the index of %s gives %lld", label, len, repo, e->size);
        return 0;
    }
    if (memcmp(digest, e->sha256, sizeof digest) != 0) {
        snprintf(err, errlen, "the SHA-256 digest of %s differs from the index of %s", label, repo);
        return 0;
    }
    return 1;
}

/* repo_fetch downloads the archive of e to dest and checks it against the
 * index. */
int repo_fetch(const struct repo_config *c, const struct index_entry *e, const char *dest)
{
    const struct repo *r = &c->repos[e->repo];
    char url[PKG_URL_MAX + PKG_PATH_MAX + 2], err[400];
    snprintf(url, sizeof url, "%s/%s", r->url, e->path);
    if (fetch(url, dest, c->timeout, e->size, err, sizeof err) < 0)
        return report(e->m.name, "%s", err);
    int m = file_matches(e, dest, e->path, r->name, err, sizeof err);
    if (m <= 0) {
        unlink(dest);
        return report(e->m.name, "%s", err);
    }
    return 0;
}

static int update_one(const struct repo_config *c, int i)
{
    const struct repo *r = &c->repos[i];
    char dir[PKG_PATH_MAX], index_new[PKG_PATH_MAX], sig_new[PKG_PATH_MAX], path[PKG_PATH_MAX];
    char url[PKG_URL_MAX + 16], err[400];
    repo_dir(dir, sizeof dir, r);
    if (mkdir_all(dir) < 0)
        return report(r->name, "%s: %s", dir, strerror(errno));
    path_join(index_new, sizeof index_new, dir, "index.new");
    path_join(sig_new, sizeof sig_new, dir, "index.sig.new");
    snprintf(url, sizeof url, "%s/index", r->url);
    if (fetch(url, index_new, c->timeout, INDEX_MAX, err, sizeof err) < 0)
        return report(r->name, "%s", err);
    snprintf(url, sizeof url, "%s/index.sig", r->url);
    if (fetch(url, sig_new, c->timeout, SIG_MAX, err, sizeof err) < 0) {
        unlink(index_new);
        return report(r->name, "%s", err);
    }
    uint8_t *text = NULL, *sig = NULL;
    size_t len, siglen;
    struct index ix = {0};
    int status = -1;
    if (read_file(index_new, &text, &len) < 0 || read_file(sig_new, &sig, &siglen) < 0)
        snprintf(err, sizeof err, "cannot read the fetched index: %s", strerror(errno));
    else if (index_verify(text, len, (char *)sig, err, sizeof err) == 0) {
        if (memchr(text, '\0', len))
            snprintf(err, sizeof err, "the index contains a zero byte");
        else if (index_parse(&ix, i, (char *)text, err, sizeof err) == 0)
            status = 0;
    }
    free(text);
    free(sig);
    if (status == 0) {
        path_join(path, sizeof path, dir, "index");
        if (rename(index_new, path) < 0)
            status = -1;
        path_join(path, sizeof path, dir, "index.sig");
        if (status == 0 && rename(sig_new, path) < 0)
            status = -1;
        path_join(path, sizeof path, dir, "url");
        char line[PKG_URL_MAX + 2];
        int n = snprintf(line, sizeof line, "%s\n", r->url);
        if (status == 0 && write_file(path, (const uint8_t *)line, (size_t)n) < 0)
            status = -1;
        if (status < 0)
            snprintf(err, sizeof err, "cannot store the index in %s: %s", dir, strerror(errno));
    }
    if (status < 0) {
        unlink(index_new);
        unlink(sig_new);
        index_free(&ix);
        return report(r->name, "%s", err);
    }
    printf("%s: %d package%s from %s\n", r->name, ix.n, ix.n == 1 ? "" : "s", r->url);
    index_free(&ix);
    return 0;
}

int cmd_update(void)
{
    struct repo_config c;
    if (config_read(&c) < 0)
        return -1;
    int r = db_lock();
    if (r < 0)
        return report(NULL, "cannot lock %s/lib/pkg: %s", prefix, strerror(-r));
    int status = 0;
    for (int i = 0; i < c.nrepos; i++)
        if (update_one(&c, i) < 0)
            status = -1;
    db_unlock();
    return status;
}

static int cmp_entries(const void *a, const void *b)
{
    const struct index_entry *x = a, *y = b;
    int c = strcmp(x->m.name, y->m.name);
    if (c == 0)
        c = version_cmp(x->m.version, y->m.version);
    return c ? c : x->repo - y->repo;
}

/* cmd_search prints every entry whose name or summary contains the
 * pattern, one line of name, version, repository and summary each. */
int cmd_search(int argc, char **argv)
{
    struct repo_config c;
    struct index ix;
    if (argc > 1)
        return report(NULL, "search takes at most one pattern");
    if (config_read(&c) < 0)
        return -1;
    int loaded = index_load(&c, &ix, 0);
    if (ix.n)
        qsort(ix.entries, (size_t)ix.n, sizeof *ix.entries, cmp_entries);
    for (int i = 0; i < ix.n; i++) {
        const struct index_entry *e = &ix.entries[i];
        if (argc == 1 && !strstr(e->m.name, argv[0]) && !strstr(e->m.summary, argv[0]))
            continue;
        printf("%s %s %s %s\n", e->m.name, e->m.version, c.repos[e->repo].name, e->m.summary);
    }
    index_free(&ix);
    return loaded == c.nrepos ? 0 : -1;
}
