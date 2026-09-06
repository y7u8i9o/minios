/* Non-PIC data access makes the linker emit a COPY relocation. Its source
 * contains a pointer, so copying before the DSO is relocated fails here. */
#include <stdint.h>
extern int ld_fixture_value(void);
extern int *ld_fixture_pointer;
extern void *ld_fixture_relro_slot(void);
int main(int argc, char **argv)
{
    if (argc > 1 && argv[1][0] == 'r') {
        *(volatile uintptr_t *)ld_fixture_relro_slot() = 0;
        return 96;                  /* a writable RELRO page is a failure */
    }
    return ld_fixture_value() == 42 && *ld_fixture_pointer == 42 ? 0 : 1;
}
