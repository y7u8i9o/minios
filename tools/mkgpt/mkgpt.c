/* mkgpt: write a GPT partitioned disk image.
 *
 *   mkgpt [--disk-uuid UUID] IMAGE SIZE_MB PARTITION...
 *   mkgpt -l IMAGE
 *
 * The same source is built for minios as /usr/bin/part, where IMAGE is
 * usually a block device such as /dev/vdb. SIZE_MB 0 stands for the current
 * size of the existing file or device, which is then written in place
 * without truncating or creating it. A device cannot be resized, thus a
 * nonzero SIZE_MB is an error for one. The option -l reads the table of
 * IMAGE, checks the header and array CRCs (falling back to the backup header
 * at the last sector when the primary fails) and prints the lines described
 * below.
 *
 * PARTITION is TYPE:SIZE[:FILE[:UUID]]. TYPE is one of bios, esp, swap,
 * root-x86_64, root-aarch64, home and linux. SIZE is a number of MiB, or
 * rest for all remaining space, which only the last partition may use. FILE
 * is an image whose bytes are copied to the start of the partition and must
 * fit in it, and is copied in chunks of at most 1 MiB. UUID is the unique partition GUID, random when omitted.
 *
 * No buffer of the size of the disk is allocated. A new image is created
 * with its full size by ftruncate and then only the protective MBR, both
 * headers, both arrays and the FILE contents are written. On an existing
 * target those regions are written in full as well, and the rest of the
 * disk is left as it is. After a table was written to a block device, the
 * kernel is asked to read it again (BLKRRPART, only in the minios build,
 * which defines MINIOS_TARGET). A failure of that request is a warning,
 * except for EBUSY, which means that the root or swap lies on the disk and
 * is an error with exit status 1.
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
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#ifdef MINIOS_TARGET
#include <sys/ioctl.h>
#endif

#define SECTOR      512
#define ALIGN_SECT  2048
#define NENTRIES    128
#define ENTRY_SIZE  128
#define ARRAY_SECT  (NENTRIES * ENTRY_SIZE / SECTOR)
#define MAX_PARTS   NENTRIES
#define COPY_CHUNK  (1u << 20)      /* the most that is read from a FILE at once */

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

static const char *prog = "mkgpt";

static __attribute__((noreturn)) void die(const char *fmt, const char *arg)
{
    fprintf(stderr, "%s: ", prog);
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

static uint32_t get32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t get64(const uint8_t *p)
{
    return get32(p) | (uint64_t)get32(p + 4) << 32;
}

/* Print the line of one partition, shared by the creating and the list mode. */
static void print_part(int n, const char *type, uint64_t first, uint64_t last, const uint8_t uuid[16])
{
    char u[37];
    guid_format(uuid, u);
    printf("%d %s %llu %llu %s\n", n, type, (unsigned long long)first, (unsigned long long)last, u);
}

/* Write n sectors of buf at sector number lba. */
static int write_sectors(FILE *f, uint64_t lba, const uint8_t *buf, size_t n)
{
    if (fseeko(f, (off_t)(lba * SECTOR), SEEK_SET) != 0)
        return -1;
    return fwrite(buf, SECTOR, n, f) == n ? 0 : -1;
}

/* Read the sectors [lba, lba + n) of the open image into buf. */
static int read_sectors(FILE *f, uint64_t lba, size_t n, uint8_t *buf)
{
    if (fseeko(f, (off_t)(lba * SECTOR), SEEK_SET) != 0)
        return -1;
    return fread(buf, SECTOR, n, f) == n ? 0 : -1;
}

/* Read the header at lba and the entry array it names, and check both CRCs.
 * Returns 0 and fills hdr and array when the table is intact. */
static int read_table(FILE *f, uint64_t total, uint64_t lba, uint8_t hdr[SECTOR], uint8_t *array)
{
    if (lba >= total || read_sectors(f, lba, 1, hdr) != 0)
        return -1;
    if (memcmp(hdr, "EFI PART", 8) != 0)
        return -1;
    uint32_t size = get32(hdr + 12);
    if (size < 92 || size > SECTOR)
        return -1;
    uint8_t copy[SECTOR];
    memcpy(copy, hdr, SECTOR);
    put32(copy + 16, 0);
    if (crc32(copy, size) != get32(hdr + 16))
        return -1;
    if (get32(hdr + 80) != NENTRIES || get32(hdr + 84) != ENTRY_SIZE)
        return -1;
    uint64_t alba = get64(hdr + 72);
    if (alba >= total || alba + ARRAY_SECT > total)
        return -1;
    if (read_sectors(f, alba, ARRAY_SECT, array) != 0)
        return -1;
    if (crc32(array, NENTRIES * ENTRY_SIZE) != get32(hdr + 88))
        return -1;
    return 0;
}

static int list_table(const char *path)
{
    struct stat st;
    if (stat(path, &st) < 0)
        die("cannot open '%s'", path);
    uint64_t total = (uint64_t)st.st_size / SECTOR;
    FILE *f = fopen(path, "rb");
    if (!f)
        die("cannot open '%s'", path);
    uint8_t hdr[SECTOR];
    uint8_t *array = malloc(NENTRIES * ENTRY_SIZE);
    if (!array)
        die("%s", "out of memory");
    if (read_table(f, total, 1, hdr, array) != 0 &&
        (total < 2 || read_table(f, total, total - 1, hdr, array) != 0))
        die("no valid partition table in '%s'", path);
    fclose(f);
    for (int i = 0; i < NENTRIES; i++) {
        const uint8_t *e = array + i * ENTRY_SIZE;
        uint8_t zero[16] = { 0 };
        if (!memcmp(e, zero, 16))
            continue;
        const char *type = NULL;
        char guid[37];
        guid_format(e, guid);
        for (size_t k = 0; k < sizeof types / sizeof types[0]; k++) {
            uint8_t t[16];
            guid_parse(types[k].guid, t);
            if (!memcmp(t, e, 16))
                type = types[k].name;
        }
        print_part(i + 1, type ? type : guid, get64(e + 32), get64(e + 40), e + 16);
    }
    free(array);
    return 0;
}

int main(int argc, char **argv)
{
    uint8_t disk[16];
    int have_disk = 0;
    int a = 1;

    const char *slash = strrchr(argv[0], '/');
    const char *base = slash ? slash + 1 : argv[0];
    if (!strcmp(base, "part"))
        prog = "part";
    if (argc == 3 && !strcmp(argv[1], "-l"))
        return list_table(argv[2]);

    if (a + 1 < argc && !strcmp(argv[a], "--disk-uuid")) {
        guid_parse(argv[a + 1], disk);
        have_disk = 1;
        a += 2;
    }
    if (argc - a < 3 || argc - a - 2 > MAX_PARTS) {
        fprintf(stderr, "usage: %s [--disk-uuid UUID] IMAGE SIZE_MB PARTITION...\n"
                        "       %s -l IMAGE\n", prog, prog);
        return 1;
    }
    if (!have_disk)
        guid_random(disk);
    const char *path = argv[a];
    int existing = 0;       /* write in place, without creating or truncating */
    int is_device = 0;      /* the target is a block device */
    uint64_t total;
    if (!strcmp(argv[a + 1], "0")) {
        struct stat st;
        if (stat(path, &st) < 0)
            die("cannot open '%s'", path);
        if (!S_ISBLK(st.st_mode) && !S_ISREG(st.st_mode))
            die("'%s' is neither a block device nor a regular file", path);
        total = (uint64_t)st.st_size / SECTOR;
        existing = 1;
        is_device = S_ISBLK(st.st_mode);
    } else {
        struct stat st;
        if (stat(path, &st) == 0 && S_ISBLK(st.st_mode))
            die("the size of the device '%s' cannot be changed, use 0", path);
        total = parse_mib(argv[a + 1]) * ALIGN_SECT;
    }
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

    /* No buffer of the size of the disk exists. Only the table structures
     * are built in memory, and the contents of FILE arguments pass through a
     * buffer of at most COPY_CHUNK bytes. */
    /* A missing or oversized FILE is found before the target is touched. */
    for (int i = 0; i < np; i++) {
        struct stat fst;
        if (!parts[i].file)
            continue;
        if (stat(parts[i].file, &fst) < 0)
            die("cannot open '%s'", parts[i].file);
        if (S_ISREG(fst.st_mode) && (uint64_t)fst.st_size > parts[i].sectors * SECTOR)
            die("'%s' does not fit in its partition", parts[i].file);
    }
    uint8_t *array = calloc(NENTRIES, ENTRY_SIZE);
    uint8_t *chunk = malloc(COPY_CHUNK);
    if (!array || !chunk)
        die("%s", "out of memory");

    FILE *out = fopen(path, existing ? "r+b" : "wb");
    if (!out)
        die("cannot create '%s'", path);
    /* A new image gets its full size first and reads as zeros wherever
     * nothing is written. On an existing target, every region of the table
     * is written in full below, since old contents are still there. */
    if (!existing && (fflush(out) != 0 || ftruncate(fileno(out), (long)(total * SECTOR)) != 0))
        die("cannot set the size of '%s'", path);

    for (int i = 0; i < np; i++) {
        struct part *p = &parts[i];
        if (!p->file)
            continue;
        FILE *f = fopen(p->file, "rb");
        if (!f)
            die("cannot open '%s'", p->file);
        uint64_t cap = p->sectors * SECTOR;
        uint64_t off = 0;
        for (;;) {
            size_t want = cap - off < COPY_CHUNK ? (size_t)(cap - off) : COPY_CHUNK;
            size_t got = want ? fread(chunk, 1, want, f) : 0;
            if (got == 0)
                break;
            /* The last piece is padded with zeros to a whole sector. */
            size_t padded = (got + SECTOR - 1) / SECTOR * SECTOR;
            memset(chunk + got, 0, padded - got);
            if (fseeko(out, (off_t)(p->first * SECTOR + off), SEEK_SET) != 0 ||
                fwrite(chunk, 1, padded, out) != padded)
                die("write to '%s' failed", path);
            off += got;
        }
        if (off == cap && fgetc(f) != EOF)
            die("'%s' does not fit in its partition", p->file);
        fclose(f);
    }
    free(chunk);

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

    uint8_t mbr[SECTOR] = { 0 };
    mbr[446 + 4] = 0xEE;
    mbr[446 + 1] = 0x00; mbr[446 + 2] = 0x02; mbr[446 + 3] = 0x00;
    mbr[446 + 5] = 0xFF; mbr[446 + 6] = 0xFF; mbr[446 + 7] = 0xFF;
    put32(mbr + 446 + 8, 1);
    put32(mbr + 446 + 12, total - 1 > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)(total - 1));
    mbr[510] = 0x55;
    mbr[511] = 0xAA;

    uint64_t backup_array = total - 1 - ARRAY_SECT;
    uint8_t primary[SECTOR], backup[SECTOR];
    write_header(primary, 1, total - 1, last_usable, disk, 2, array_crc);
    write_header(backup, total - 1, 1, last_usable, disk, backup_array, array_crc);

    /* The backup comes first and the protective MBR last, thus a table that
     * is cut short by a failure is never taken for a complete one. */
    if (write_sectors(out, backup_array, array, ARRAY_SECT) != 0 ||
        write_sectors(out, total - 1, backup, 1) != 0 ||
        write_sectors(out, 2, array, ARRAY_SECT) != 0 ||
        write_sectors(out, 1, primary, 1) != 0 ||
        write_sectors(out, 0, mbr, 1) != 0)
        die("write to '%s' failed", path);
    free(array);
    if (fflush(out) != 0)
        die("write to '%s' failed", path);

    /* A device is told to read the table again. */
    int status = 0;
#ifdef MINIOS_TARGET
    if (is_device && ioctl(fileno(out), BLKRRPART) < 0) {
        if (errno == EBUSY) {
            fprintf(stderr, "%s: '%s' cannot be read again, the root or swap lies on this disk\n", prog, path);
            status = 1;
        } else {
            fprintf(stderr, "%s: warning: the kernel did not read the table of '%s' again: %s\n",
                    prog, path, strerror(errno));
        }
    }
#else
    (void)is_device;
#endif
    if (fclose(out) != 0)
        die("write to '%s' failed", path);

    for (int i = 0; i < np; i++)
        print_part(i + 1, parts[i].type, parts[i].first, parts[i].last, parts[i].uuid);
    return status;
}
