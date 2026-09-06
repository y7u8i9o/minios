/* The library of the package installer test. Built twice: with
 * pkgfix_value, which the program calls, and without it, so that a
 * package shipping the second build fails the symbol check. */
#ifdef WITHOUT_VALUE
int pkgfix_other(void) { return 1; }
#else
int pkgfix_value(void) { return 42; }
#endif
