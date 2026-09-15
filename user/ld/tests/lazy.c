/* Linked with -z lazy: the procedure linkage table entries are bound on
 * first call. The global offset table shows it: before any call every
 * jump slot points into this program's own table, after the calls the
 * used slots point into the libraries above 4 GiB. The variadic call
 * with doubles goes through the resolver with its vector arguments. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

extern double tls_sum(int count, ...);
extern int *tls_counter_address(void);
/* The dynamic section names the table (DT_PLTGOT) and the size of the
 * jump slot relocations (DT_PLTRELSZ, 24 bytes each). */
extern const struct { long tag; unsigned long value; } _DYNAMIC[] __attribute__((visibility("hidden")));

static int slots_in_program(void)
{
    const uintptr_t *got = NULL;
    size_t slots = 0;
    for (size_t i = 0; _DYNAMIC[i].tag; i++) {
        if (_DYNAMIC[i].tag == 3)
            got = (const uintptr_t *)_DYNAMIC[i].value;
        if (_DYNAMIC[i].tag == 2)
            slots = _DYNAMIC[i].value / 24;
    }
    int n = 0;
    for (size_t i = 0; got && i < slots; i++)
        if (got[3 + i] < 0x1000000UL)
            n++;
    return n;
}

int main(void)
{
    int before = slots_in_program();
    double sum = tls_sum(3, 1.5, 2.25, 0.25);
    int counter = *tls_counter_address();
    int after = slots_in_program();
    char text[64];
    snprintf(text, sizeof text, "lazy %d %d %.2f %d", before > 0, after < before, sum, counter);
    puts(text);
    return 0;
}
