/* The registry of codecs. The first lookup opens the modules in
 * CODEC_DIR, or in the directory named by CODEC_PATH, with dlopen in the
 * order of their file names, and the modules remain loaded. Lookups by
 * name, MIME type, extension and content walk the codecs in that order.
 *
 * Locking. The tables below are filled once under init_once and are read
 * without a lock afterwards. codec_register takes register_lock, which
 * also protects the tables while they grow. A program that registers
 * modules must do it before it starts threads that call the lookup
 * functions. */
#include <codec/codec.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#ifndef CODEC_BUILTIN
#include <dlfcn.h>
#endif

#define MAX_MODULES 32
#define MAX_CODECS 64

static struct {
    const struct codec_module *module;
    char path[96];
} modules[MAX_MODULES];
static int nmodules;
static const struct codec *codecs[MAX_CODECS];
static int ncodecs;
static pthread_once_t init_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t register_lock = PTHREAD_MUTEX_INITIALIZER;

static int add_module(const struct codec_module *m, const char *path)
{
    if (!m || m->abi != CODEC_MODULE_ABI || m->count < 0)
        return -EINVAL;
    pthread_mutex_lock(&register_lock);
    int err = 0;
    if (nmodules == MAX_MODULES || ncodecs + m->count > MAX_CODECS) {
        err = -ENOSPC;
    } else {
        modules[nmodules].module = m;
        snprintf(modules[nmodules].path, sizeof modules[nmodules].path, "%s", path ? path : "");
        nmodules++;
        for (int i = 0; i < m->count; i++)
            codecs[ncodecs++] = &m->codecs[i];
    }
    pthread_mutex_unlock(&register_lock);
    return err;
}

#ifdef CODEC_BUILTIN
/* The modules compiled into the program, in the order of their names.
 * A new module must be added here as well. */
extern const struct codec_module codec_module_bmp, codec_module_flac, codec_module_gif, codec_module_jpeg,
    codec_module_mp3, codec_module_opus, codec_module_png, codec_module_svg, codec_module_vorbis, codec_module_wav;

static void load_all(void)
{
    const struct codec_module *builtin[] = { &codec_module_bmp, &codec_module_flac, &codec_module_gif,
                                             &codec_module_jpeg, &codec_module_mp3, &codec_module_opus, &codec_module_png,
                                             &codec_module_svg, &codec_module_vorbis, &codec_module_wav };
    for (size_t i = 0; i < sizeof builtin / sizeof builtin[0]; i++)
        add_module(builtin[i], "builtin");
}
#else
static int by_name(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Every *.so of the directory, sorted, so that the order of the codecs
 * and therefore the winner of equal probe scores does not depend on the
 * order of the directory. */
static void load_all(void)
{
    const char *dir = getenv("CODEC_PATH");
    if (!dir || !dir[0])
        dir = CODEC_DIR;
    DIR *d = opendir(dir);
    if (!d)
        return;
    char *names[MAX_MODULES];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < MAX_MODULES) {
        size_t len = strlen(e->d_name);
        if (len > 3 && strcmp(e->d_name + len - 3, ".so") == 0 && (names[n] = strdup(e->d_name)))
            n++;
    }
    closedir(d);
    qsort(names, (size_t)n, sizeof names[0], by_name);
    for (int i = 0; i < n; i++) {
        char path[96];
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        void *h = dlopen(path, RTLD_NOW);
        const struct codec_module *m = h ? dlsym(h, "codec_module") : NULL;
        if (!m)
            fprintf(stderr, "codec: %s: %s\n", path, h ? "no codec_module" : dlerror());
        else if (m->abi != CODEC_MODULE_ABI)
            fprintf(stderr, "codec: %s: module ABI %d, not %d\n", path, m->abi, CODEC_MODULE_ABI);
        else if (add_module(m, path) < 0)
            fprintf(stderr, "codec: %s: too many modules or codecs\n", path);
        else
            h = NULL;                   /* remains loaded */
        if (h)
            dlclose(h);
        free(names[i]);
    }
}
#endif

static void init(void)
{
    pthread_once(&init_once, load_all);
}

int codec_register(const struct codec_module *m, const char *path)
{
    init();
    return add_module(m, path);
}

int codec_count(void)
{
    init();
    return ncodecs;
}

const struct codec *codec_get(int index)
{
    init();
    return index >= 0 && index < ncodecs ? codecs[index] : NULL;
}

int codec_module_count(void)
{
    init();
    return nmodules;
}

const struct codec_module *codec_module_get(int index)
{
    init();
    return index >= 0 && index < nmodules ? modules[index].module : NULL;
}

const char *codec_module_path(int index)
{
    init();
    return index >= 0 && index < nmodules ? modules[index].path : NULL;
}

static int matches(const struct codec *c, enum codec_kind kind, int caps)
{
    return (!kind || c->kind == kind) && (c->caps & caps) == caps;
}

/* Whether word occurs in a list separated by spaces, without regard to
 * case. */
static int in_list(const char *list, const char *word)
{
    size_t n = strlen(word);
    for (const char *p = list; p && *p;) {
        while (*p == ' ')
            p++;
        size_t k = strcspn(p, " ");
        if (k == n && strncasecmp(p, word, n) == 0)
            return 1;
        p += k;
    }
    return 0;
}

const struct codec *codec_find(const char *name)
{
    init();
    for (int i = 0; i < ncodecs; i++)
        if (strcasecmp(codecs[i]->name, name) == 0)
            return codecs[i];
    return NULL;
}

const struct codec *codec_for_mime(enum codec_kind kind, const char *mime, int caps)
{
    if (!mime)
        return NULL;
    init();
    for (int i = 0; i < ncodecs; i++)
        if (matches(codecs[i], kind, caps) && in_list(codecs[i]->mime_types, mime))
            return codecs[i];
    return NULL;
}

const struct codec *codec_for_path(enum codec_kind kind, const char *path, int caps)
{
    const char *slash = path ? strrchr(path, '/') : NULL;
    const char *dot = path ? strrchr(slash ? slash : path, '.') : NULL;
    if (!dot || !dot[1])
        return NULL;
    init();
    for (int i = 0; i < ncodecs; i++)
        if (matches(codecs[i], kind, caps) && in_list(codecs[i]->extensions, dot + 1))
            return codecs[i];
    return NULL;
}

const struct codec *codec_for_data(enum codec_kind kind, const uint8_t *data, size_t len, int caps)
{
    init();
    if (len > CODEC_PROBE_LEN)
        len = CODEC_PROBE_LEN;
    const struct codec *best = NULL;
    int score = 0;
    for (int i = 0; i < ncodecs; i++) {
        if (!matches(codecs[i], kind, caps) || !codecs[i]->probe)
            continue;
        int s = codecs[i]->probe(data, len);
        if (s > score) {
            score = s;
            best = codecs[i];
        }
    }
    return best;
}

const struct codec *codec_identify(enum codec_kind kind, const uint8_t *data, size_t len, const char *path,
                                   int caps)
{
    const struct codec *c = data ? codec_for_data(kind, data, len, caps) : NULL;
    return c ? c : codec_for_path(kind, path, caps);
}
