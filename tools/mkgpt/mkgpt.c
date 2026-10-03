/* mkgpt: write a GPT partitioned disk image.
 *
 *   mkgpt [--disk-uuid UUID] IMAGE SIZE_MB PARTITION...
 *
 * PARTITION is TYPE:SIZE[:FILE[:UUID]]. TYPE is one of bios, esp, swap,
 * root-x86_64, root-aarch64, home and linux. SIZE is a number of MiB, or
 * rest for all remaining space, which only the last partition may use. FILE
 * is an image whose bytes are copied to the start of the partition and must
 * fit in it. UUID is the unique partition GUID, random when omitted.
 *
 * The sector size is 512. The image holds a protective MBR, the primary
 * header at LBA 1 with its array of 128 entries, and the backup array and
 * header at the end of the disk. Partitions start at LBA 2048 and every
 * start is aligned to 1 MiB. For each partition one line
 * "N TYPE FIRST_LBA LAST_LBA UUID" is printed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

#define SECTOR      512
#define ALIGN_SECT  2048
#define NENTRIES    128
#define ENTRY_SIZE  128
#define ARRAY_SECT  (NENTRIES * ENTRY_SIZE / SECTOR)
#define MAX_PARTS   NENTRIES

struct type_name {
    const char *name;
    const char *guid;
};

static const struct type_name types[] = {
    { "bios",         "21686148-6449-6E6F-744E-656564454649" },
    { "esp",          "C12A7328-F81F-11D2-BA4B-00A0C93EC93B" },
    { "swap",         "0657FD6D-A4AB-43C4-84E5-0933C84B4F4F" },
    { "root-x86_64",  "4F68BCE3-E8CD-4DB1-96E7-FBCAF984B709" },
    { "root-aarch64", "B921B045-1DF0-41C3-AF44-4C6F280D3FAE" },
    { "home",         "933AC7E1-2EB4-4F13-B844-0E14E2AEF915" },
    { "linux",        "0FC63DAF-8483-4772-8E79-3D69D8477DE4" },
};

struct part {
    const char *type;
    uint8_t type_guid[16];          /* on-disk form */
    uint8_t uuid[16];               /* on-disk form */
    uint64_t sectors;               /* 0 for rest until laid out */
    int is_rest;
    const char *file;
    uint64_t first, last;
};

static __attribute__((noreturn)) void die(const char *fmt, const char *arg)
{
    fprintf(stderr, "mkgpt: ");
    fprintf(stderr, fmt, arg);
    fputc('\n', stderr);
    exit(1);
}

static uint32_t crc32(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int i = 0; i < 8; i++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
static void put64(uint8_t *p, uint64_t v) { put32(p, v); put32(p + 4, v >> 32); }

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Parse a standard string GUID into the mixed endian on-disk form. */
static void guid_parse(const char *s, uint8_t out[16])
{
    uint8_t b[16];
    size_t pos = 0;
    int nb = 0;
    for (; s[pos]; pos++) {
        if (pos == 8 || pos == 13 || pos == 18 || pos == 23) {
            if (s[pos] != '-')
                die("malformed UUID '%s'", s);
            continue;
        }
        int hi = hexval(s[pos]);
        if (hi < 0 || !s[pos + 1] || nb >= 16)
            die("malformed UUID '%s'", s);
        int lo;
        pos++;
        if (pos == 8 || pos == 13 || pos == 18 || pos == 23 || (lo = hexval(s[pos])) < 0)
            die("malformed UUID '%s'", s);
        b[nb++] = (uint8_t)(hi << 4 | lo);
    }
    if (nb != 16 || pos != 36)
        die("malformed UUID '%s'", s);
    out[0] = b[3]; out[1] = b[2]; out[2] = b[1]; out[3] = b[0];
    out[4] = b[5]; out[5] = b[4];
    out[6] = b[7]; out[7] = b[6];
    memcpy(out + 8, b + 8, 8);
}

static void guid_format(const uint8_t g[16], char out[37])
{
    snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             g[3], g[2], g[1], g[0], g[5], g[4], g[7], g[6], g[8], g[9],
             g[10], g[11], g[12], g[13], g[14], g[15]);
}

/* A random version 4 GUID from /dev/urandom. */
static void guid_random(uint8_t g[16])
{
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f || fread(g, 1, 16, f) != 16)
        die("cannot read %s", "/dev/urandom");
    fclose(f);
    g[7] = (g[7] & 0x0F) | 0x40;    /* version, in the time_hi field */
    g[8] = (g[8] & 0x3F) | 0x80;    /* variant */
}

static uint64_t parse_mib(const char *s)
{
    char *end;
    if (!*s || !isdigit((unsigned char)*s))
        die("bad size '%s'", s);
    unsigned long long v = strtoull(s, &end, 10);
    if (*end || v == 0 || v > (1ull << 32))
        die("bad size '%s'", s);
    return v;
}

static void parse_part(const char *arg, struct part *p)
{
    char *copy = strdup(arg);
    char *f[4] = { 0 };
    int n = 0;
    char *c = copy;
    if (!copy)
        die("%s", "out of memory");
    for (;;) {
        f[n++] = c;
        c = strchr(c, ':');
        if (!c)
            break;
        *c++ = 0;
        if (n == 4)
            die("too many fields in '%s'", arg);
    }
    memset(p, 0, sizeof *p);
    size_t i;
    for (i = 0; i < sizeof types / sizeof types[0]; i++)
        if (!strcmp(f[0], types[i].name))
            break;
    if (i == sizeof types / sizeof types[0])
        die("unknown partition type '%s'", f[0]);
    p->type = types[i].name;
    guid_parse(types[i].guid, p->type_guid);
    if (n < 2)
        die("missing size in '%s'", arg);
    if (!strcmp(f[1], "rest"))
        p->is_rest = 1;
    else
        p->sectors = parse_mib(f[1]) * ALIGN_SECT;
    if (n >= 3 && *f[2])
        p->file = f[2];
    if (n >= 4)
        guid_parse(f[3], p->uuid);
    else
        guid_random(p->uuid);
}

static void write_header(uint8_t *h, uint64_t my, uint64_t alt, uint64_t last_usable,
                         const uint8_t disk[16], uint64_t array_lba, uint32_t array_crc)
{
    memset(h, 0, SECTOR);
    memcpy(h, "EFI PART", 8);
    put32(h + 8, 0x00010000);
    put32(h + 12, 92);
    put64(h + 24, my);
    put64(h + 32, alt);
    put64(h + 40, 34);
    put64(h + 48, last_usable);
    memcpy(h + 56, disk, 16);
    put64(h + 72, array_lba);
    put32(h + 80, NENTRIES);
    put32(h + 84, ENTRY_SIZE);
    put32(h + 88, array_crc);
    put32(h + 16, crc32(h, 92));
}

int main(int argc, char **argv)
{
    uint8_t disk[16];
    int have_disk = 0;
    int a = 1;

    if (a + 1 < argc && !strcmp(argv[a], "--disk-uuid")) {
        guid_parse(argv[a + 1], disk);
        have_disk = 1;
        a += 2;
    }
    if (argc - a < 3 || argc - a - 2 > MAX_PARTS) {
        fprintf(stderr, "usage: mkgpt [--disk-uuid UUID] IMAGE SIZE_MB PARTITION...\n");
        return 1;
    }
    if (!have_disk)
        guid_random(disk);
    const char *path = argv[a];
    uint64_t total = parse_mib(argv[a + 1]) * ALIGN_SECT;
    int np = argc - a - 2;
    if (total < 2 * ALIGN_SECT)
        die("%s", "image too small");
    uint64_t last_usable = total - 34;

    struct part *parts = calloc(np, sizeof *parts);
    if (!parts)
        die("%s", "out of memory");
    for (int i = 0; i < np; i++) {
        parse_part(argv[a + 2 + i], &parts[i]);
        if (parts[i].is_rest && i != np - 1)
            die("'rest' is allowed only for the last partition%s", "");
    }

    uint64_t next = ALIGN_SECT;
    for (int i = 0; i < np; i++) {
        struct part *p = &parts[i];
        p->first = next;
        if (p->is_rest) {
            if (p->first > last_usable)
                die("no space left for the partition%s", "");
            p->last = last_usable;
            p->sectors = p->last - p->first + 1;
        } else {
            p->last = p->first + p->sectors - 1;
            if (p->last > last_usable)
                die("partitions do not fit in the image%s", "");
        }
        next = (p->last + 1 + ALIGN_SECT - 1) / ALIGN_SECT * ALIGN_SECT;
    }

    uint8_t *img = calloc(total, SECTOR);
    if (!img)
        die("cannot allocate the image%s", "");

    for (int i = 0; i < np; i++) {
        struct part *p = &parts[i];
        if (!p->file)
            continue;
        FILE *f = fopen(p->file, "rb");
        if (!f)
            die("cannot open '%s'", p->file);
        uint64_t cap = p->sectors * SECTOR;
        uint8_t *dst = img + p->first * SECTOR;
        size_t got = fread(dst, 1, cap, f);
        if (got == cap && fgetc(f) != EOF)
            die("'%s' does not fit in its partition", p->file);
        fclose(f);
    }

    uint8_t *array = img + 2 * SECTOR;
    for (int i = 0; i < np; i++) {
        uint8_t *e = array + i * ENTRY_SIZE;
        struct part *p = &parts[i];
        memcpy(e, p->type_guid, 16);
        memcpy(e + 16, p->uuid, 16);
        put64(e + 32, p->first);
        put64(e + 40, p->last);
        for (int k = 0; p->type[k] && k < 36; k++)
            put16(e + 56 + 2 * k, (uint8_t)p->type[k]);
    }
    uint32_t array_crc = crc32(array, NENTRIES * ENTRY_SIZE);

    uint8_t *mbr = img;
    mbr[446 + 4] = 0xEE;
    mbr[446 + 1] = 0x00; mbr[446 + 2] = 0x02; mbr[446 + 3] = 0x00;
    mbr[446 + 5] = 0xFF; mbr[446 + 6] = 0xFF; mbr[446 + 7] = 0xFF;
    put32(mbr + 446 + 8, 1);
    put32(mbr + 446 + 12, total - 1 > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)(total - 1));
    mbr[510] = 0x55;
    mbr[511] = 0xAA;

    uint64_t backup_array = total - 1 - ARRAY_SECT;
    memcpy(img + backup_array * SECTOR, array, NENTRIES * ENTRY_SIZE);
    write_header(img + SECTOR, 1, total - 1, last_usable, disk, 2, array_crc);
    write_header(img + (total - 1) * SECTOR, total - 1, 1, last_usable, disk, backup_array,
                 array_crc);

    FILE *out = fopen(path, "wb");
    if (!out)
        die("cannot create '%s'", path);
    if (fwrite(img, SECTOR, total, out) != total || fclose(out) != 0)
        die("write to '%s' failed", path);

    for (int i = 0; i < np; i++) {
        char u[37];
        guid_format(parts[i].uuid, u);
        printf("%d %s %llu %llu %s\n", i + 1, parts[i].type,
               (unsigned long long)parts[i].first, (unsigned long long)parts[i].last, u);
    }
    return 0;
}
