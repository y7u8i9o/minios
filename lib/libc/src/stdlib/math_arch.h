#pragma once

/* Primitives that the architecture implements for the math library
 * (lib/libc/arch/<arch>/). */

/* The partial remainder of x / y with the quotient rounded toward zero
 * (fmod) or to the nearest integer (remainder, nearest = 1). x and y are
 * finite and y is not zero. */
double __math_partial_remainder(double x, double y, int nearest);
