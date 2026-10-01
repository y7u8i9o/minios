/* A3: the parts of the user ABI that libc and the loader take from the
 * architecture (docs/design/arch.md): setjmp, the floating point
 * environment, the SIMD lane rules, the thread pointer, the stack of a new
 * thread and the initial-exec TLS offsets computed by the loader. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <setjmp.h>
#include <fenv.h>
#include <math.h>
#include <pthread.h>
#include <minios/simd.h>

static int failures;

#define CHECK(condition, ...) do { \
    if (!(condition)) { \
        failures++; \
        printf("abitest: FAIL " __VA_ARGS__); \
        printf("\n"); \
    } \
} while (0)

static jmp_buf env;
static volatile int depth;

static void __attribute__((noinline)) dive(int n)
{
    volatile char pad[64];
    pad[0] = (char)n;
    depth = n;
    if (n == 5)
        longjmp(env, n + pad[0]);
    if (n < 5)
        dive(n + 1);
}

static void test_setjmp(void)
{
    volatile int local = 42;
    int r = setjmp(env);
    if (r == 0) {
        dive(1);
        CHECK(0, "dive returned");
        return;
    }
    CHECK(r == 10 && depth == 5 && local == 42, "longjmp returned %d at depth %d, local %d", r, depth, local);
    CHECK(setjmp(env) == 0, "a direct setjmp returned nonzero");
}

static void test_fenv(void)
{
    CHECK(fegetround() == FE_TONEAREST, "default rounding mode %d", fegetround());
    volatile double one = 1.0, three = 3.0;
    volatile double third = one / three;
    CHECK(fesetround(FE_UPWARD) == 0, "FE_UPWARD");
    volatile double up = one / three;
    CHECK(fesetround(FE_DOWNWARD) == 0, "FE_DOWNWARD");
    volatile double down = one / three;
    fesetround(FE_TONEAREST);
    CHECK(up > down && (third == up || third == down), "directed rounding: %a %a %a", down, third, up);
    feclearexcept(FE_ALL_EXCEPT);
    volatile double zero = 0.0;
    volatile double inf = 1.0 / zero;
    CHECK(isinf(inf) && fetestexcept(FE_DIVBYZERO), "division by zero not flagged");
    feclearexcept(FE_ALL_EXCEPT);
    CHECK(!fetestexcept(FE_ALL_EXCEPT), "flags not cleared");
}

static void test_simd(void)
{
    simd_f32x4 a = simd_set_f32x4(1.0f, NAN, 3.0f, -0.0f);
    simd_f32x4 b = simd_set_f32x4(2.0f, 5.0f, NAN, 0.0f);
    simd_f32x4 lo = simd_min_f32x4(a, b), hi = simd_max_f32x4(a, b);
    /* An unordered or equal lane gives the second operand. */
    CHECK(lo[0] == 1.0f && lo[1] == 5.0f && isnan(lo[2]) && lo[3] == 0.0f && !signbit(lo[3]),
          "min lanes %g %g %g %g", lo[0], lo[1], lo[2], lo[3]);
    CHECK(hi[0] == 2.0f && hi[1] == 5.0f && isnan(hi[2]) && !signbit(hi[3]),
          "max lanes %g %g %g %g", hi[0], hi[1], hi[2], hi[3]);
    float in[7] = { 0, 1, 4, 9, 16, 25, 36 }, out[7];
    simd_sqrt_f32(out, in, 7);
    for (int i = 0; i < 7; i++)
        CHECK(out[i] == (float)i, "sqrt lane %d gave %g", i, out[i]);
}

static __thread int tls_counter = 7;
static __thread char tls_buffer[32] = "initial";

struct thread_result {
    uintptr_t aligned_local;
    int counter;
    char buffer[32];
};

static void *thread_main(void *arg)
{
    struct thread_result *res = arg;
    _Alignas(16) volatile char probe[16];
    res->aligned_local = (uintptr_t)probe;
    res->counter = tls_counter;
    tls_counter = 99;
    memcpy(res->buffer, tls_buffer, sizeof res->buffer);
    return NULL;
}

static void test_threads(void)
{
    struct thread_result res;
    memset(&res, 0, sizeof res);
    pthread_t t;
    CHECK(pthread_create(&t, NULL, thread_main, &res) == 0, "pthread_create");
    pthread_join(t, NULL);
    CHECK(res.aligned_local % 16 == 0, "16 byte aligned local at %lx in a new thread", (unsigned long)res.aligned_local);
    CHECK(res.counter == 7 && strcmp(res.buffer, "initial") == 0,
          "new thread saw TLS %d \"%s\"", res.counter, res.buffer);
    CHECK(tls_counter == 7, "the main thread's TLS changed to %d", tls_counter);
    CHECK(pthread_equal(pthread_self(), pthread_self()), "pthread_self");
}

int main(void)
{
    test_setjmp();
    test_fenv();
    test_simd();
    test_threads();
    printf("abitest: %d failures\n", failures);
    return failures ? 1 : 0;
}
