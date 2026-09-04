#include <locale.h>
#include <string.h>

/* MiniOS deliberately has one process-wide locale.  UTF-8 is accepted by
 * the multibyte conversion routines, while collation, case mapping,
 * numbers, currency and dates retain the deterministic C locale rules. */
static char c_name[] = "C";
static char empty[] = "";
static char decimal[] = ".";

static struct lconv c_locale = {
    .decimal_point = decimal,
    .thousands_sep = empty,
    .grouping = empty,
    .int_curr_symbol = empty,
    .currency_symbol = empty,
    .mon_decimal_point = empty,
    .mon_thousands_sep = empty,
    .mon_grouping = empty,
    .positive_sign = empty,
    .negative_sign = empty,
    .int_frac_digits = __SCHAR_MAX__,
    .frac_digits = __SCHAR_MAX__,
    .p_cs_precedes = __SCHAR_MAX__,
    .p_sep_by_space = __SCHAR_MAX__,
    .n_cs_precedes = __SCHAR_MAX__,
    .n_sep_by_space = __SCHAR_MAX__,
    .p_sign_posn = __SCHAR_MAX__,
    .n_sign_posn = __SCHAR_MAX__,
    .int_p_cs_precedes = __SCHAR_MAX__,
    .int_p_sep_by_space = __SCHAR_MAX__,
    .int_n_cs_precedes = __SCHAR_MAX__,
    .int_n_sep_by_space = __SCHAR_MAX__,
    .int_p_sign_posn = __SCHAR_MAX__,
    .int_n_sign_posn = __SCHAR_MAX__,
};

static int valid_category(int category)
{
    return category >= LC_ALL && category <= LC_MESSAGES;
}

char *setlocale(int category, const char *locale)
{
    if (!valid_category(category))
        return 0;
    if (!locale || !*locale || strcmp(locale, "C") == 0 ||
        strcmp(locale, "POSIX") == 0)
        return c_name;
    return 0;
}

struct lconv *localeconv(void)
{
    return &c_locale;
}
