#include <lib/fdt.h>
#include <lib/string.h>

/* The format of the Devicetree Specification: a big-endian header, a
 * structure block of tokens and a strings block of property names. */

#define FDT_MAGIC      0xd00dfeed
#define FDT_BEGIN_NODE 1
#define FDT_END_NODE   2
#define FDT_PROP       3
#define FDT_NOP        4
#define FDT_END        9
#define FDT_MAX_DEPTH  16

struct fdt_header {
    uint32_t magic, totalsize, off_dt_struct, off_dt_strings, off_mem_rsvmap;
    uint32_t version, last_comp_version, boot_cpuid_phys, size_dt_strings, size_dt_struct;
};

static uint32_t be32(const void *p)
{
    const uint8_t *b = p;
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

uint32_t fdt_cell(const void *value, int index)
{
    return be32((const uint8_t *)value + 4 * index);
}

static const struct fdt_header *header(const void *blob)
{
    return blob;
}

bool fdt_valid(const void *blob)
{
    const struct fdt_header *h = header(blob);
    return blob && be32(&h->magic) == FDT_MAGIC && be32(&h->last_comp_version) <= 17;
}

static const uint8_t *structure(const void *blob)
{
    return (const uint8_t *)blob + be32(&header(blob)->off_dt_struct);
}

static const char *string_at(const void *blob, uint32_t offset)
{
    return (const char *)blob + be32(&header(blob)->off_dt_strings) + offset;
}

/* The offset of the token after the property or node name at off. */
static int skip_name(const uint8_t *s, int off)
{
    return (int)ALIGN_UP((uintptr_t)off + strlen((const char *)s + off) + 1, 4);
}

const void *fdt_prop(const void *blob, const struct fdt_node *node, const char *name, int *len)
{
    const uint8_t *s = structure(blob);
    int off = skip_name(s, node->offset + 4);
    for (;;) {
        uint32_t token = be32(s + off);
        if (token == FDT_NOP) {
            off += 4;
            continue;
        }
        if (token != FDT_PROP)
            return NULL;
        uint32_t plen = be32(s + off + 4);
        uint32_t nameoff = be32(s + off + 8);
        if (strcmp(string_at(blob, nameoff), name) == 0) {
            if (len)
                *len = (int)plen;
            return s + off + 12;
        }
        off = (int)ALIGN_UP((uintptr_t)off + 12 + plen, 4);
    }
}

static bool list_contains(const char *list, int len, const char *want)
{
    for (int i = 0; i < len;) {
        if (strcmp(list + i, want) == 0)
            return true;
        i += (int)strlen(list + i) + 1;
    }
    return false;
}

/* The next node after *node, or from the start of the tree when
 * node->offset is negative, for which match returns true. */
static bool find_node(const void *blob, struct fdt_node *node,
                      bool (*match)(const void *blob, const struct fdt_node *n, const void *arg), const void *arg)
{
    const uint8_t *s = structure(blob);
    uint32_t addr_cells[FDT_MAX_DEPTH], size_cells[FDT_MAX_DEPTH];
    int depth = -1;
    int start = node->offset;
    for (int off = 0;;) {
        uint32_t token = be32(s + off);
        if (token == FDT_END)
            return false;
        if (token == FDT_NOP) {
            off += 4;
            continue;
        }
        if (token == FDT_END_NODE) {
            depth--;
            off += 4;
            continue;
        }
        if (token == FDT_PROP) {
            off = (int)ALIGN_UP((uintptr_t)off + 12 + be32(s + off + 4), 4);
            continue;
        }
        if (token != FDT_BEGIN_NODE || depth + 1 >= FDT_MAX_DEPTH)
            return false;
        /* A node: the cells its children use default to 2 and 1. */
        depth++;
        struct fdt_node here = {
            .offset = off,
            .addr_cells = depth ? addr_cells[depth - 1] : 2,
            .size_cells = depth ? size_cells[depth - 1] : 1,
        };
        int len;
        const void *v = fdt_prop(blob, &here, "#address-cells", &len);
        addr_cells[depth] = v && len == 4 ? be32(v) : 2;
        v = fdt_prop(blob, &here, "#size-cells", &len);
        size_cells[depth] = v && len == 4 ? be32(v) : 1;
        if (off > start && match(blob, &here, arg)) {
            *node = here;
            return true;
        }
        off = skip_name(s, off + 4);
    }
}

static bool match_compatible(const void *blob, const struct fdt_node *n, const void *arg)
{
    int len;
    const char *list = fdt_prop(blob, n, "compatible", &len);
    return list && list_contains(list, len, arg);
}

bool fdt_find_compatible(const void *blob, const char *compat, struct fdt_node *node)
{
    return find_node(blob, node, match_compatible, compat);
}

bool fdt_is_compatible(const void *blob, const struct fdt_node *node, const char *compat)
{
    return match_compatible(blob, node, compat);
}

static bool match_property(const void *blob, const struct fdt_node *n, const void *arg)
{
    return fdt_prop(blob, n, arg, NULL) != NULL;
}

bool fdt_find_property(const void *blob, const char *name, struct fdt_node *node)
{
    return find_node(blob, node, match_property, name);
}

static bool match_phandle(const void *blob, const struct fdt_node *n, const void *arg)
{
    int len;
    const void *v = fdt_prop(blob, n, "phandle", &len);
    return v && len == 4 && be32(v) == *(const uint32_t *)arg;
}

bool fdt_find_phandle(const void *blob, uint32_t phandle, struct fdt_node *node)
{
    node->offset = -1;
    return find_node(blob, node, match_phandle, &phandle);
}

bool fdt_reg(const void *blob, const struct fdt_node *node, int index, uint64_t *addr, uint64_t *size)
{
    int len;
    const uint8_t *reg = fdt_prop(blob, node, "reg", &len);
    int cells = (int)(node->addr_cells + node->size_cells);
    if (!reg || cells == 0 || (index + 1) * cells * 4 > len)
        return false;
    const uint8_t *entry = reg + index * cells * 4;
    uint64_t a = 0, s = 0;
    for (uint32_t i = 0; i < node->addr_cells; i++)
        a = a << 32 | be32(entry + 4 * i);
    for (uint32_t i = 0; i < node->size_cells; i++)
        s = s << 32 | be32(entry + 4 * (node->addr_cells + i));
    *addr = a;
    *size = s;
    return true;
}
