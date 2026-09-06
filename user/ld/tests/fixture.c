/* The fixture generator mutates this DSO's headers and dynamic tables.
 * It contains a real initializer, BSS, exported data, and relocations so
 * rejection cases exercise the same structures as working libraries. */
unsigned char ld_fixture_bss[4096 + 31];
int ld_fixture_data = 42;
int *ld_fixture_pointer = &ld_fixture_data;
int *const ld_fixture_relro = &ld_fixture_data;
static int initialized;

/* Return the address through a function so the executable cannot receive
 * a COPY relocation for this read-only slot. The parent checks that an
 * attempted write into the library's RELRO page terminates with SIGSEGV. */
void *ld_fixture_relro_slot(void) { return (void *)&ld_fixture_relro; }

__attribute__((constructor)) static void fixture_init(void) { initialized = 1; }

int ld_fixture_value(void)
{
    if (!initialized)
        return -1;
    for (unsigned i = 0; i < sizeof ld_fixture_bss; i++)
        if (ld_fixture_bss[i] != 0)
            return -2;
    ld_fixture_bss[0] = 1;
    ld_fixture_bss[sizeof ld_fixture_bss - 1] = 2;
    return ld_fixture_data;
}
