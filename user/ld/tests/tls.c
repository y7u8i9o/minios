/* libldtls.so: thread local variables in a library loaded at start, so
 * that its blocks are part of the static layout. The program reaches
 * them through the general dynamic model (__tls_get_addr) and through a
 * variable declared initial-exec, which the loader binds to a fixed
 * offset from the thread pointer. */
#include <stdint.h>

__thread int tls_counter = 5;
__thread char tls_buffer[64] = "image";
static __thread int tls_ie __attribute__((tls_model("initial-exec"))) = 7;
static __thread int tls_ld;                 /* local dynamic, zero initialized */

int *tls_counter_address(void)
{
    return &tls_counter;
}

char *tls_buffer_address(void)
{
    return tls_buffer;
}

int tls_ie_get(void)
{
    return tls_ie;
}

void tls_ie_set(int value)
{
    tls_ie = value;
}

int tls_ld_next(void)
{
    return ++tls_ld;
}

/* For the lazy binding test: a variadic sum of doubles, so that a first
 * call through the resolver must preserve the vector registers. */
double tls_sum(int count, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, count);
    double total = 0;
    for (int i = 0; i < count; i++)
        total += __builtin_va_arg(ap, double);
    __builtin_va_end(ap);
    return total;
}
