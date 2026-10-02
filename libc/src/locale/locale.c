/* Named locales (L1, docs/design/locale.md).
 *
 * A locale other than C and C.UTF-8 is a text file
 * /usr/share/i18n/locales/ll_CC with one item per line: a key, then a
 * quoted string, a quoted list separated by semicolons, or a number.
 * setlocale and newlocale load a file once and store the parsed copy in a
 * list for the life of the process.  Every locale decodes UTF-8, and
 * nl_langinfo(CODESET) is "UTF-8" in every locale. */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include "locale_impl.h"
#include "../thread/tcb.h"

#define LOCALE_DIR "/usr/share/i18n/locales"
#define LOCALE_FILE_MAX 16384

const struct locale_data __c_locale_data = {
    .name = "C",
    .str = {
        [CODESET] = "UTF-8", [RADIXCHAR] = ".", [THOUSEP] = "",
        [D_T_FMT] = "%a %b %e %H:%M:%S %Y", [D_FMT] = "%m/%d/%y", [T_FMT] = "%H:%M:%S",
        [T_FMT_AMPM] = "%I:%M:%S %p", [AM_STR] = "AM", [PM_STR] = "PM",
        [DAY_1] = "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday",
        [ABDAY_1] = "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat",
        [MON_1] = "January", "February", "March", "April", "May", "June", "July", "August", "September",
        "October", "November", "December",
        [ABMON_1] = "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
        [ALTMON_1] = "January", "February", "March", "April", "May", "June", "July", "August", "September",
        "October", "November", "December",
        [_NL_ABALTMON_1] = "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
        [ERA] = "", [ERA_D_FMT] = "", [ALT_DIGITS] = "", [ERA_D_T_FMT] = "", [ERA_T_FMT] = "",
        [YESEXPR] = "^[yY]", [NOEXPR] = "^[nN]", [CRNCYSTR] = "-", [_DATE_FMT] = "%a %b %e %H:%M:%S %Z %Y",
        [LI_GROUPING] = "", [LI_INT_CURR_SYMBOL] = "", [LI_CURRENCY_SYMBOL] = "", [LI_MON_DECIMAL_POINT] = "",
        [LI_MON_THOUSANDS_SEP] = "", [LI_MON_GROUPING] = "", [LI_POSITIVE_SIGN] = "", [LI_NEGATIVE_SIGN] = "",
        [LI_YESSTR] = "yes", [LI_NOSTR] = "no", [LI_COLLATE] = "codepoint", [LI_COLLATE_AFTER] = "",
        [LI_LANGUAGE_NAME] = "C",
        [LI_TERRITORY_NAME] = "",
    },
    .num = { -1, -1, -1, -1, -1, -1, -1, -1 },
};

/* C.UTF-8 differs from C in its name only. */
static struct locale_data c_utf8_data;

struct __locale_struct __global_locale = { {
    &__c_locale_data, &__c_locale_data, &__c_locale_data, &__c_locale_data,
    &__c_locale_data, &__c_locale_data, &__c_locale_data,
} };

/* The list of loaded locale files and the buffers returned by setlocale
 * are protected by lock. */
static struct __libc_lock lock = __LIBC_LOCK_INIT;
static struct locale_data *loaded;
static char query_buf[300];

static const char *const category_names[] = {
    "LC_ALL", "LC_COLLATE", "LC_CTYPE", "LC_MONETARY", "LC_NUMERIC", "LC_TIME", "LC_MESSAGES",
};

/* The functions below read locale files. */

enum kind { K_STR, K_LIST, K_NUM, K_GROUP };

static const struct {
    const char *key;
    int item;
    enum kind kind;
    int count;
} keys[] = {
    { "decimal_point", RADIXCHAR, K_STR, 1 },
    { "thousands_sep", THOUSEP, K_STR, 1 },
    { "grouping", LI_GROUPING, K_GROUP, 1 },
    { "int_curr_symbol", LI_INT_CURR_SYMBOL, K_STR, 1 },
    { "currency_symbol", LI_CURRENCY_SYMBOL, K_STR, 1 },
    { "mon_decimal_point", LI_MON_DECIMAL_POINT, K_STR, 1 },
    { "mon_thousands_sep", LI_MON_THOUSANDS_SEP, K_STR, 1 },
    { "mon_grouping", LI_MON_GROUPING, K_GROUP, 1 },
    { "positive_sign", LI_POSITIVE_SIGN, K_STR, 1 },
    { "negative_sign", LI_NEGATIVE_SIGN, K_STR, 1 },
    { "int_frac_digits", LN_INT_FRAC_DIGITS, K_NUM, 1 },
    { "frac_digits", LN_FRAC_DIGITS, K_NUM, 1 },
    { "p_cs_precedes", LN_P_CS_PRECEDES, K_NUM, 1 },
    { "p_sep_by_space", LN_P_SEP_BY_SPACE, K_NUM, 1 },
    { "n_cs_precedes", LN_N_CS_PRECEDES, K_NUM, 1 },
    { "n_sep_by_space", LN_N_SEP_BY_SPACE, K_NUM, 1 },
    { "p_sign_posn", LN_P_SIGN_POSN, K_NUM, 1 },
    { "n_sign_posn", LN_N_SIGN_POSN, K_NUM, 1 },
    { "abday", ABDAY_1, K_LIST, 7 },
    { "day", DAY_1, K_LIST, 7 },
    { "abmon", ABMON_1, K_LIST, 12 },
    { "mon", MON_1, K_LIST, 12 },
    { "alt_mon", ALTMON_1, K_LIST, 12 },
    { "ab_alt_mon", _NL_ABALTMON_1, K_LIST, 12 },
    { "am_pm", AM_STR, K_LIST, 2 },
    { "d_t_fmt", D_T_FMT, K_STR, 1 },
    { "d_fmt", D_FMT, K_STR, 1 },
    { "t_fmt", T_FMT, K_STR, 1 },
    { "t_fmt_ampm", T_FMT_AMPM, K_STR, 1 },
    { "date_fmt", _DATE_FMT, K_STR, 1 },
    { "yesexpr", YESEXPR, K_STR, 1 },
    { "noexpr", NOEXPR, K_STR, 1 },
    { "yesstr", LI_YESSTR, K_STR, 1 },
    { "nostr", LI_NOSTR, K_STR, 1 },
    { "collate", LI_COLLATE, K_STR, 1 },
    { "collate_after", LI_COLLATE_AFTER, K_STR, 1 },
    { "language_name", LI_LANGUAGE_NAME, K_STR, 1 },
    { "territory_name", LI_TERRITORY_NAME, K_STR, 1 },
};

static int hex_value(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static char *put_utf8(char *out, unsigned cp)
{
    if (cp < 0x80) {
        *out++ = (char)cp;
    } else if (cp < 0x800) {
        *out++ = (char)(0xc0 | cp >> 6);
        *out++ = (char)(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
        *out++ = (char)(0xe0 | cp >> 12);
        *out++ = (char)(0x80 | (cp >> 6 & 0x3f));
        *out++ = (char)(0x80 | (cp & 0x3f));
    } else {
        *out++ = (char)(0xf0 | cp >> 18);
        *out++ = (char)(0x80 | (cp >> 12 & 0x3f));
        *out++ = (char)(0x80 | (cp >> 6 & 0x3f));
        *out++ = (char)(0x80 | (cp & 0x3f));
    }
    return out;
}

/* unquote decodes the quoted string at *p in place, with the escapes \\,
 * \" and \uXXXX, and returns it.  *p moves past the closing quote. */
static char *unquote(char **p)
{
    char *in = *p + 1, *out = in, *start = in;
    while (*in && *in != '"' && *in != '\n') {
        if (*in == '\\' && in[1] == 'u') {
            unsigned cp = 0;
            int k = 2;
            for (; k < 8 && hex_value(in[k]) >= 0; k++)
                cp = cp * 16 + (unsigned)hex_value(in[k]);
            out = put_utf8(out, cp);
            in += k;
        } else if (*in == '\\' && in[1]) {
            *out++ = in[1];
            in += 2;
        } else {
            *out++ = *in++;
        }
    }
    if (*in == '"')
        in++;
    *p = in;
    *out = '\0';
    return start;
}

/* group converts a list of group sizes such as "3;2" into the byte string
 * of struct lconv, in place. */
static char *group(char *s)
{
    char *out = s, *start = s;
    while (*s) {
        long n = strtol(s, &s, 10);
        if (n > 0 && n < 127)
            *out++ = (char)n;
        while (*s && !isdigit((unsigned char)*s))
            s++;
    }
    *out = '\0';
    return start;
}

static void parse(struct locale_data *d, char *text)
{
    char *p = text;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            p++;
        if (!*p)
            break;
        if (*p == '#' || !islower((unsigned char)*p)) {
            while (*p && *p != '\n')
                p++;
            continue;
        }
        char *key = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n')
            p++;
        char *key_end = p;
        while (*p == ' ' || *p == '\t')
            p++;
        char saved = *key_end;
        *key_end = '\0';
        size_t k = 0;
        while (k < sizeof keys / sizeof keys[0] && strcmp(keys[k].key, key) != 0)
            k++;
        *key_end = saved;
        if (k == sizeof keys / sizeof keys[0] || (*p != '"' && keys[k].kind != K_NUM)) {
            while (*p && *p != '\n')
                p++;
            continue;
        }
        if (keys[k].kind == K_NUM) {
            d->num[keys[k].item] = (signed char)strtol(p, &p, 10);
        } else {
            char *value = unquote(&p);
            if (keys[k].kind == K_GROUP) {
                d->str[keys[k].item] = group(value);
            } else if (keys[k].kind == K_STR) {
                d->str[keys[k].item] = value;
            } else {
                for (int i = 0; i < keys[k].count; i++) {
                    d->str[keys[k].item + i] = value;
                    char *semi = strchr(value, ';');
                    if (!semi)
                        break;
                    *semi = '\0';
                    value = semi + 1;
                }
            }
        }
        while (*p && *p != '\n')
            p++;
    }
}

/* finish fills the items that the file does not set and computes
 * CRNCYSTR from the currency symbol and its position. */
static void finish(struct locale_data *d, char *crncy, size_t size)
{
    for (int i = 0; i < 12; i++) {
        if (!d->str[ALTMON_1 + i])
            d->str[ALTMON_1 + i] = d->str[MON_1 + i];
        if (!d->str[_NL_ABALTMON_1 + i])
            d->str[_NL_ABALTMON_1 + i] = d->str[ABMON_1 + i];
    }
    for (int i = 0; i < LI_COUNT; i++)
        if (!d->str[i])
            d->str[i] = __c_locale_data.str[i];
    const char *sym = d->str[LI_CURRENCY_SYMBOL];
    if (*sym) {
        crncy[0] = d->num[LN_P_CS_PRECEDES] == 0 ? '+' : '-';
        strlcpy(crncy + 1, sym, size - 1);
        d->str[CRNCYSTR] = crncy;
    }
    d->str[CODESET] = "UTF-8";
    d->collate_latin = strcmp(d->str[LI_COLLATE], "latin") == 0;
}

/* read_file loads /usr/share/i18n/locales/<file>. */
static struct locale_data *read_file(const char *file, const char *name)
{
    char path[64];
    strlcpy(path, LOCALE_DIR "/", sizeof path);
    strlcat(path, file, sizeof path);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return NULL;
    struct locale_data *d = calloc(1, sizeof *d + LOCALE_FILE_MAX + 64);
    if (!d) {
        close(fd);
        return NULL;
    }
    char *text = (char *)(d + 1), *crncy = text + LOCALE_FILE_MAX;
    ssize_t n, total = 0;
    while (total < LOCALE_FILE_MAX - 1 && (n = read(fd, text + total, (size_t)(LOCALE_FILE_MAX - 1 - total))) > 0)
        total += n;
    close(fd);
    text[total] = '\0';
    memset(d->num, -1, sizeof d->num);
    strlcpy(d->name, name, sizeof d->name);
    parse(d, text);
    finish(d, crncy, 64);
    return d;
}

/* The functions below resolve locale names. */

static int is_utf8_codeset(const char *s, size_t n)
{
    return (n == 5 && strncasecmp(s, "UTF-8", 5) == 0) || (n == 4 && strncasecmp(s, "utf8", 4) == 0);
}

/* first_file_of finds the first locale file of a language, in the order
 * of the names. */
static int first_file_of(const char *lang, char *out, size_t size)
{
    DIR *dir = opendir(LOCALE_DIR);
    if (!dir)
        return -1;
    size_t n = strlen(lang);
    out[0] = '\0';
    struct dirent *e;
    while ((e = readdir(dir)) != NULL)
        if (strncmp(e->d_name, lang, n) == 0 && e->d_name[n] == '_' &&
            (!out[0] || strcmp(e->d_name, out) < 0))
            strlcpy(out, e->d_name, size);
    closedir(dir);
    return out[0] ? 0 : -1;
}

/* load returns the loaded locale of a name: C, POSIX, C.UTF-8, ll_CC with
 * an optional codeset UTF-8 or utf8, or ll.  The caller has acquired lock. */
static const struct locale_data *load(const char *name)
{
    if (strcmp(name, "C") == 0 || strcmp(name, "POSIX") == 0)
        return &__c_locale_data;
    if (strncmp(name, "C.", 2) == 0 && is_utf8_codeset(name + 2, strlen(name + 2))) {
        if (!c_utf8_data.name[0]) {
            c_utf8_data = __c_locale_data;
            strlcpy(c_utf8_data.name, "C.UTF-8", sizeof c_utf8_data.name);
        }
        return &c_utf8_data;
    }
    const char *p = name;
    size_t lang = 0;
    while (lang < 3 && islower((unsigned char)p[lang]))
        lang++;
    if (lang < 2)
        return NULL;
    p += lang;
    char file[16];
    if (*p == '_') {
        if (!isupper((unsigned char)p[1]) || !isupper((unsigned char)p[2]))
            return NULL;
        memcpy(file, name, lang + 3);
        file[lang + 3] = '\0';
        p += 3;
    } else {
        char l[4];
        memcpy(l, name, lang);
        l[lang] = '\0';
        if (first_file_of(l, file, sizeof file) < 0)
            return NULL;
    }
    if (*p == '.') {
        const char *end = strchr(p, '@');
        size_t n = end ? (size_t)(end - p - 1) : strlen(p + 1);
        if (!is_utf8_codeset(p + 1, n))
            return NULL;
        p += 1 + n;
    }
    if (*p)
        return NULL;
    char canonical[40];
    strlcpy(canonical, file, sizeof canonical);
    strlcat(canonical, ".UTF-8", sizeof canonical);
    for (struct locale_data *d = loaded; d; d = d->next)
        if (strcmp(d->name, canonical) == 0)
            return d;
    struct locale_data *d = read_file(file, canonical);
    if (d) {
        d->next = loaded;
        loaded = d;
    }
    return d;
}

/* from_environment returns the name that the environment gives a
 * category: LC_ALL, then LC_<category>, then LANG, then C. */
static const char *from_environment(int category)
{
    const char *v = getenv("LC_ALL");
    if (v && *v)
        return v;
    v = getenv(category_names[category]);
    if (v && *v)
        return v;
    v = getenv("LANG");
    return v && *v ? v : "C";
}

/* resolve fills set[] with the loaded locales that name gives the
 * categories of mask.  A name of the form LC_CTYPE=...;LC_TIME=... sets
 * each listed category.  Returns 0, or -1 when a name is unknown. */
static int resolve(int mask, const char *name, const struct locale_data **set)
{
    if (strchr(name, '=')) {
        char copy[300];
        strlcpy(copy, name, sizeof copy);
        for (char *part = copy, *next; part; part = next) {
            next = strchr(part, ';');
            if (next)
                *next++ = '\0';
            char *eq = strchr(part, '=');
            if (!eq)
                return -1;
            *eq = '\0';
            for (int c = LC_COLLATE; c <= LC_MESSAGES; c++)
                if (strcmp(part, category_names[c]) == 0 && (mask & (1 << c)) && !(set[c] = load(eq + 1)))
                    return -1;
        }
        return 0;
    }
    for (int c = LC_COLLATE; c <= LC_MESSAGES; c++)
        if (mask & (1 << c)) {
            set[c] = load(*name ? name : from_environment(c));
            if (!set[c])
                return -1;
        }
    return 0;
}

/* query writes the name of a category of loc, or for LC_ALL the common
 * name or the list of every category, into query_buf. */
static char *query(struct __locale_struct *loc, int category)
{
    if (category != LC_ALL) {
        strlcpy(query_buf, loc->cat[category]->name, sizeof query_buf);
        return query_buf;
    }
    int same = 1;
    for (int c = LC_COLLATE + 1; c <= LC_MESSAGES; c++)
        same &= loc->cat[c] == loc->cat[LC_COLLATE];
    if (same) {
        strlcpy(query_buf, loc->cat[LC_COLLATE]->name, sizeof query_buf);
        return query_buf;
    }
    query_buf[0] = '\0';
    for (int c = LC_COLLATE; c <= LC_MESSAGES; c++) {
        if (c != LC_COLLATE)
            strlcat(query_buf, ";", sizeof query_buf);
        strlcat(query_buf, category_names[c], sizeof query_buf);
        strlcat(query_buf, "=", sizeof query_buf);
        strlcat(query_buf, loc->cat[c]->name, sizeof query_buf);
    }
    return query_buf;
}

char *setlocale(int category, const char *name)
{
    if (category < LC_ALL || category > LC_MESSAGES)
        return NULL;
    __libc_lock_lock(&lock);
    char *result = NULL;
    if (!name) {
        result = query(&__global_locale, category);
    } else {
        const struct locale_data *set[LC_MESSAGES + 1] = { 0 };
        int mask = category == LC_ALL ? LC_ALL_MASK : 1 << category;
        if (resolve(mask, name, set) == 0) {
            for (int c = LC_COLLATE; c <= LC_MESSAGES; c++)
                if (set[c])
                    __global_locale.cat[c] = set[c];
            result = query(&__global_locale, category);
        }
    }
    __libc_lock_unlock(&lock);
    return result;
}

/* The functions below give access to the items of a locale. */

struct __locale_struct *__locale_current(void)
{
    struct __locale_struct *loc = __pthread_current()->locale;
    return loc ? loc : &__global_locale;
}

int __locale_item_category(nl_item item)
{
    if (item == CODESET)
        return LC_CTYPE;
    if (item == RADIXCHAR || item == THOUSEP || item == LI_GROUPING)
        return LC_NUMERIC;
    if (item == YESEXPR || item == NOEXPR || item == LI_YESSTR || item == LI_NOSTR || item == LI_LANGUAGE_NAME ||
        item == LI_TERRITORY_NAME)
        return LC_MESSAGES;
    if (item == CRNCYSTR || (item >= LI_INT_CURR_SYMBOL && item <= LI_NEGATIVE_SIGN))
        return LC_MONETARY;
    if (item == LI_COLLATE || item == LI_COLLATE_AFTER)
        return LC_COLLATE;
    return LC_TIME;
}

const char *__locale_item(struct __locale_struct *loc, nl_item item)
{
    if (item < 0 || item >= LI_COUNT)
        return "";
    if (loc == LC_GLOBAL_LOCALE)
        loc = &__global_locale;
    return loc->cat[__locale_item_category(item)]->str[item];
}

char *nl_langinfo_l(nl_item item, locale_t loc)
{
    return (char *)__locale_item(loc, item);
}

char *nl_langinfo(nl_item item)
{
    return (char *)__locale_item(__locale_current(), item);
}

struct lconv *localeconv(void)
{
    static struct lconv conv;
    struct __locale_struct *loc = __locale_current();
    const struct locale_data *num = loc->cat[LC_NUMERIC], *mon = loc->cat[LC_MONETARY];
    conv.decimal_point = (char *)num->str[RADIXCHAR];
    conv.thousands_sep = (char *)num->str[THOUSEP];
    conv.grouping = (char *)num->str[LI_GROUPING];
    conv.int_curr_symbol = (char *)mon->str[LI_INT_CURR_SYMBOL];
    conv.currency_symbol = (char *)mon->str[LI_CURRENCY_SYMBOL];
    conv.mon_decimal_point = (char *)mon->str[LI_MON_DECIMAL_POINT];
    conv.mon_thousands_sep = (char *)mon->str[LI_MON_THOUSANDS_SEP];
    conv.mon_grouping = (char *)mon->str[LI_MON_GROUPING];
    conv.positive_sign = (char *)mon->str[LI_POSITIVE_SIGN];
    conv.negative_sign = (char *)mon->str[LI_NEGATIVE_SIGN];
    /* A value of -1 in the locale is CHAR_MAX, "not available". */
    char *fields[] = { &conv.int_frac_digits, &conv.frac_digits, &conv.p_cs_precedes, &conv.p_sep_by_space,
                       &conv.n_cs_precedes, &conv.n_sep_by_space, &conv.p_sign_posn, &conv.n_sign_posn };
    for (int i = 0; i < LN_COUNT; i++)
        *fields[i] = mon->num[i] < 0 ? __SCHAR_MAX__ : (char)mon->num[i];
    conv.int_p_cs_precedes = conv.p_cs_precedes;
    conv.int_p_sep_by_space = conv.p_sep_by_space;
    conv.int_n_cs_precedes = conv.n_cs_precedes;
    conv.int_n_sep_by_space = conv.n_sep_by_space;
    conv.int_p_sign_posn = conv.p_sign_posn;
    conv.int_n_sign_posn = conv.n_sign_posn;
    return &conv;
}

/* The functions below implement locale objects. */

locale_t newlocale(int mask, const char *name, locale_t base)
{
    if ((mask & ~LC_ALL_MASK) || !name || base == LC_GLOBAL_LOCALE) {
        errno = EINVAL;
        return NULL;
    }
    const struct locale_data *set[LC_MESSAGES + 1] = { 0 };
    __libc_lock_lock(&lock);
    int r = resolve(mask, name, set);
    __libc_lock_unlock(&lock);
    if (r < 0) {
        errno = ENOENT;
        return NULL;
    }
    locale_t loc = base;
    if (!loc) {
        loc = malloc(sizeof *loc);
        if (!loc) {
            errno = ENOMEM;
            return NULL;
        }
        for (int c = 0; c <= LC_MESSAGES; c++)
            loc->cat[c] = &__c_locale_data;
    }
    for (int c = LC_COLLATE; c <= LC_MESSAGES; c++)
        if (set[c])
            loc->cat[c] = set[c];
    return loc;
}

locale_t duplocale(locale_t loc)
{
    locale_t copy = malloc(sizeof *copy);
    if (!copy) {
        errno = ENOMEM;
        return NULL;
    }
    *copy = loc == LC_GLOBAL_LOCALE ? __global_locale : *loc;
    return copy;
}

void freelocale(locale_t loc)
{
    if (loc && loc != LC_GLOBAL_LOCALE && loc != &__global_locale)
        free(loc);
}

locale_t uselocale(locale_t loc)
{
    struct pthread *t = __pthread_current();
    locale_t old = t->locale ? t->locale : LC_GLOBAL_LOCALE;
    if (loc == LC_GLOBAL_LOCALE)
        t->locale = NULL;
    else if (loc)
        t->locale = loc;
    return old;
}
