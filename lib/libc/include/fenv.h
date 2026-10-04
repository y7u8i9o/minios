#pragma once

#include <stdint.h>

/* The exception and rounding mode values and fenv_t are defined by the
 * architecture (bits/<arch>/fenv.h). */
#if defined(__x86_64__)
#include <bits/x86_64/fenv.h>
#elif defined(__aarch64__)
#include <bits/aarch64/fenv.h>
#else
#error "fenv.h: unsupported architecture"
#endif

int feclearexcept(int excepts);
int fegetexceptflag(fexcept_t *flag, int excepts);
int feraiseexcept(int excepts);
int fesetexceptflag(const fexcept_t *flag, int excepts);
int fetestexcept(int excepts);
int fegetround(void);
int fesetround(int mode);
int fegetenv(fenv_t *env);
int feholdexcept(fenv_t *env);
int fesetenv(const fenv_t *env);
int feupdateenv(const fenv_t *env);
