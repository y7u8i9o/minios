/* The x86_64 primitives of the math library: the SSE2 square roots, the
 * x87 partial remainder behind fmod and remainder, and the x87 arc
 * tangent behind the inverse trigonometric functions of math_extra.c. The
 * other long double functions are in math_long.c. */
#include <math.h>
#include <errno.h>
#include <stdlib/math_arch.h>

double sqrt(double x)
{
    if (x < 0.0) {
        errno = EDOM;
        return NAN;
    }
    double result;
    __asm__ volatile("sqrtsd %1, %0" : "=x"(result) : "x"(x));
    return result;
}

float sqrtf(float x)
{
    if (x < 0.0f) {
        errno = EDOM;
        return NAN;
    }
    float result;
    __asm__ volatile("sqrtss %1, %0" : "=x"(result) : "x"(x));
    return result;
}

double __math_partial_remainder(double x, double y, int nearest)
{
    double result;
    unsigned short status;
    if (nearest) {
        __asm__ volatile(
            "fldl %[divisor]\n\t"
            "fldl %[value]\n\t"
            "1: fprem1\n\t"
            "fnstsw %%ax\n\t"
            "testw $0x400, %%ax\n\t"
            "jnz 1b\n\t"
            "fstpl %[result]\n\t"
            "fstp %%st(0)"
            : [result] "=m" (result), "=&a" (status)
            : [value] "m" (x), [divisor] "m" (y)
            : "cc", "st", "st(1)");
    } else {
        __asm__ volatile(
            "fldl %[divisor]\n\t"
            "fldl %[value]\n\t"
            "1: fprem\n\t"
            "fnstsw %%ax\n\t"
            "testw $0x400, %%ax\n\t"
            "jnz 1b\n\t"
            "fstpl %[result]\n\t"
            "fstp %%st(0)"
            : [result] "=m" (result), "=&a" (status)
            : [value] "m" (x), [divisor] "m" (y)
            : "cc", "st", "st(1)");
    }
    (void)status;
    return result;
}

long double atanl(long double x)
{
    if (isnan(x))
        return x;
    long double result;
    __asm__ volatile("fldt %1; fld1; fpatan; fstpt %0"
                     : "=m" (result) : "m" (x) : "st", "st(1)");
    return result;
}

long double atan2l(long double y, long double x)
{
    if (isnan(x))
        return x;
    if (isnan(y))
        return y;
    long double result;
    __asm__ volatile("fldt %1; fldt %2; fpatan; fstpt %0"
                     : "=m" (result) : "m" (y), "m" (x) : "st", "st(1)");
    return result;
}
