#pragma once
#include <kernel.h>

/* A reader of flattened device trees (A7), for the blob that Limine passes
 * on aarch64. The reader does not modify the blob. The blob is in the
 * memory where the bootloader placed it, until that memory is reclaimed. */

/* A node found in the tree: its offset in the structure block and the
 * #address-cells and #size-cells that apply to its reg property (those of
 * its parent). */
struct fdt_node {
    int offset;
    uint32_t addr_cells, size_cells;
};

/* True if blob is a device tree of a version this reader understands. */
bool fdt_valid(const void *blob);
/* Find the next node after *node (from the start of the tree when
 * node->offset is negative) whose compatible property lists compat. */
bool fdt_find_compatible(const void *blob, const char *compat, struct fdt_node *node);
/* The value and the length of a property of a node, or NULL. */
const void *fdt_prop(const void *blob, const struct fdt_node *node, const char *name, int *len);
/* Entry index of the reg property of a node. */
bool fdt_reg(const void *blob, const struct fdt_node *node, int index, uint64_t *addr, uint64_t *size);
/* A big-endian cell of a property value. */
uint32_t fdt_cell(const void *value, int index);
