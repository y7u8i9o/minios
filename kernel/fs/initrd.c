#define KLOG_SUBSYS "initrd"
#include <fs/initrd.h>
#include <boot.h>
#include <lib/string.h>
#include <klog.h>

struct tar_header {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char checksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
};

static struct initrd_entry entries[INITRD_MAX_ENTRIES];
static size_t nentries;

static uint64_t newest_mtime;

uint64_t initrd_newest_mtime(void) { return newest_mtime; }

static size_t parse_octal(const char *s, size_t n)
{
    size_t v = 0;
    for (size_t i = 0; i < n && s[i] >= '0' && s[i] <= '7'; i++)
        v = v * 8 + (size_t)(s[i] - '0');
    return v;
}

/* Strip "./" and leading slashes, drop a trailing slash. */
static void normalize(char *dst, const char *src, size_t max)
{
    while (src[0] == '.' && src[1] == '/')
        src += 2;
    while (*src == '/')
        src++;
    size_t n = strnlen(src, max);
    while (n > 0 && src[n - 1] == '/')
        n--;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void initrd_init(void)
{
    if (!bootinfo.initrd_size) {
        klog_warn("no initrd module");
        return;
    }
    const uint8_t *p = bootinfo.initrd;
    const uint8_t *end = p + bootinfo.initrd_size;
    size_t dropped = 0;
    while (p + sizeof(struct tar_header) <= end) {
        const struct tar_header *h = (const struct tar_header *)p;
        if (h->name[0] == '\0')
            break;
        size_t size = parse_octal(h->size, sizeof h->size);
        if (nentries < INITRD_MAX_ENTRIES) {
            struct initrd_entry *e = &entries[nentries];
            char full[256];
            if (h->prefix[0]) {
                size_t pl = strnlen(h->prefix, sizeof h->prefix);
                memcpy(full, h->prefix, pl);
                full[pl] = '/';
                size_t nl = strnlen(h->name, sizeof h->name);
                memcpy(full + pl + 1, h->name, nl);
                full[pl + 1 + nl] = '\0';
            } else {
                size_t nl = strnlen(h->name, sizeof h->name);
                memcpy(full, h->name, nl);
                full[nl] = '\0';
            }
            normalize(e->name, full, INITRD_NAME_MAX);
            e->type = h->typeflag == '5' ? INITRD_DIR : h->typeflag == '2' ? INITRD_LINK : INITRD_FILE;
            e->data = p + 512;
            e->size = e->type == INITRD_DIR ? 0 : size;
            if (e->type == INITRD_LINK) {
                /* The target is the header's link name field, NUL
                 * terminated only when shorter than the field. */
                e->data = (const uint8_t *)h->linkname;
                e->size = strnlen(h->linkname, sizeof h->linkname);
            }
            e->mtime = parse_octal(h->mtime, sizeof h->mtime);
            if (e->mtime > newest_mtime)
                newest_mtime = e->mtime;
            if (e->name[0] != '\0')
                nentries++;
        } else {
            dropped++;
        }
        p += 512 + ALIGN_UP(size, 512);
    }
    if (dropped)
        klog_warn("%zu members beyond the first %d are not available", dropped, INITRD_MAX_ENTRIES);
    size_t files = 0, links = 0;
    for (size_t i = 0; i < nentries; i++) {
        files += entries[i].type == INITRD_FILE;
        links += entries[i].type == INITRD_LINK;
    }
    klog_info("tar of %lu KiB, %zu files, %zu directories and %zu symbolic links",
              bootinfo.initrd_size >> 10, files, nentries - files - links, links);
}

const struct initrd_entry *initrd_lookup(const char *path)
{
    char norm[INITRD_NAME_MAX + 1];
    normalize(norm, path, INITRD_NAME_MAX);
    for (size_t i = 0; i < nentries; i++) {
        if (strcmp(entries[i].name, norm) == 0)
            return &entries[i];
    }
    return NULL;
}

size_t initrd_count(void)
{
    return nentries;
}

const struct initrd_entry *initrd_entry(size_t index)
{
    return index < nentries ? &entries[index] : NULL;
}
