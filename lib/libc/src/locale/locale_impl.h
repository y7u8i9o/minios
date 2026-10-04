#pragma once
/* Locale internals shared by locale.c, collate.c, the number formatting
 * and conversion of stdio and stdlib, and strftime (docs/design/locale.md). */
#include <locale.h>
#include <langinfo.h>

/* String items: the nl_item values of <langinfo.h> first, then items that
 * only localeconv and the C library use. */
enum {
    LI_GROUPING = _DATE_FMT + 1,
    LI_INT_CURR_SYMBOL,
    LI_CURRENCY_SYMBOL,
    LI_MON_DECIMAL_POINT,
    LI_MON_THOUSANDS_SEP,
    LI_MON_GROUPING,
    LI_POSITIVE_SIGN,
    LI_NEGATIVE_SIGN,
    LI_YESSTR,
    LI_NOSTR,
    LI_COLLATE,
    LI_COLLATE_AFTER,
    LI_LANGUAGE_NAME,
    LI_TERRITORY_NAME,
    LI_COUNT
};

/* Numeric items of LC_MONETARY in the order of struct lconv. */
enum {
    LN_INT_FRAC_DIGITS,
    LN_FRAC_DIGITS,
    LN_P_CS_PRECEDES,
    LN_P_SEP_BY_SPACE,
    LN_N_CS_PRECEDES,
    LN_N_SEP_BY_SPACE,
    LN_P_SIGN_POSN,
    LN_N_SIGN_POSN,
    LN_COUNT
};

/* One loaded locale: the C locale, C.UTF-8, or a file of
 * /usr/share/i18n/locales.  Loaded locales are never freed. */
struct locale_data {
    char name[40];                      /* ll_CC.UTF-8, C or C.UTF-8 */
    const char *str[LI_COUNT];
    signed char num[LN_COUNT];
    int collate_latin;                  /* 1 for the three level collation */
    struct locale_data *next;           /* the list of loaded locales */
};

/* A locale object: the loaded locale of each category, indexed by LC_*. */
struct __locale_struct {
    const struct locale_data *cat[LC_MESSAGES + 1];
};

extern struct __locale_struct __global_locale;
extern const struct locale_data __c_locale_data;

/* The locale of the calling thread: the one set with uselocale, else the
 * global locale. */
struct __locale_struct *__locale_current(void);

/* The string item of a category of a locale, never NULL. */
const char *__locale_item(struct __locale_struct *loc, nl_item item);

/* The category that contains a string item. */
int __locale_item_category(nl_item item);
