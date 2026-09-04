#pragma once
#include <kernel.h>

void ksyms_init(void);
size_t ksyms_count(void);
/* Find the text symbol containing addr. Returns NULL if none. On success
 * *offset and *size describe the position within the symbol. */
const char *ksyms_lookup(uintptr_t addr, uintptr_t *offset, size_t *size);
/* Entry i of the sorted table (M41). */
const char *ksyms_entry(size_t i, uintptr_t *addr, size_t *size);
/* Print "[<addr>] name+0xoff/0xsize" or the raw address if unknown. */
void ksyms_print_addr(uintptr_t addr);
