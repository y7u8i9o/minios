/* The program of the package installer test: it loads libpkgfix.so from
 * the package prefix and prints the value the library returns. */
#include <stdio.h>

int pkgfix_value(void);

int main(void)
{
    printf("pkgfix %d\n", pkgfix_value());
    return 0;
}
