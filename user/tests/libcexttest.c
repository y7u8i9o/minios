/* Extended libc coverage: non-local jumps, locale, UTF-8 wide characters,
 * POSIX regular expressions and the C time additions. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <setjmp.h>
#include <locale.h>
#include <wchar.h>
#include <regex.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("libcexttest: FAIL " __VA_ARGS__); printf("\n"); } } while (0)

static jmp_buf jump_target;

static void jump_from_callee(int value)
{
    longjmp(jump_target, value);
}

static void test_setjmp(void)
{
    int value = setjmp(jump_target);
    if (!value)
        jump_from_callee(37);
    CHECK(value == 37, "longjmp returned %d", value);

    value = setjmp(jump_target);
    if (!value)
        jump_from_callee(0);
    CHECK(value == 1, "longjmp zero became %d", value);
}

static void test_locale(void)
{
    CHECK(strcmp(setlocale(LC_ALL, 0), "C") == 0, "locale query");
    CHECK(strcmp(setlocale(LC_CTYPE, "POSIX"), "C") == 0, "POSIX locale alias");
    CHECK(setlocale(LC_ALL, "not-a-locale") == 0, "unknown locale rejected");
    struct lconv *locale = localeconv();
    CHECK(strcmp(locale->decimal_point, ".") == 0 && !*locale->thousands_sep,
          "C numeric locale");
}

static void test_wchar(void)
{
    wchar_t text[16];
    wcscpy(text, L"wide");
    wcscat(text, L" chars");
    CHECK(wcslen(text) == 10 && wcscmp(text, L"wide chars") == 0, "wide strings");
    CHECK(wcsstr(text, L"chars") == text + 5 && wcschr(text, L' ') == text + 4,
          "wide searches");

    const char encoded[] = "A\xc3\xa9\xf0\x9f\x98\x80";
    const char *source = encoded;
    wchar_t decoded[8];
    mbstate_t state = { 0 };
    size_t count = mbsrtowcs(decoded, &source, 8, &state);
    CHECK(count == 3 && source == 0 && decoded[0] == L'A' &&
          decoded[1] == 0xe9 && decoded[2] == 0x1f600,
          "UTF-8 decode count %lu", (unsigned long)count);

    char roundtrip[16];
    const wchar_t *wide_source = decoded;
    memset(&state, 0, sizeof state);
    count = wcsrtombs(roundtrip, &wide_source, sizeof roundtrip, &state);
    CHECK(count == sizeof encoded - 1 && wide_source == 0 &&
          strcmp(roundtrip, encoded) == 0, "UTF-8 round trip");

    wchar_t wc = 0;
    memset(&state, 0, sizeof state);
    CHECK(mbrtowc(&wc, "\xc3", 1, &state) == (size_t)-2 && !mbsinit(&state),
          "partial UTF-8 lead");
    CHECK(mbrtowc(&wc, "\xa9", 1, &state) == 1 && wc == 0xe9 && mbsinit(&state),
          "partial UTF-8 completion");
    errno = 0;
    CHECK(mbrtowc(&wc, "\xc0", 1, &state) == (size_t)-1 && errno == EILSEQ,
          "invalid UTF-8 rejected");

    wchar_t *end;
    CHECK(wcstol(L" -123x", &end, 10) == -123 && *end == L'x', "wcstol");
}

static void test_regex(void)
{
    regex_t regex;
    regmatch_t match[4];
    int error = regcomp(&regex, "^(ab|a)([[:digit:]]{2,3})$", REG_EXTENDED);
    CHECK(error == 0, "compile ERE: %d", error);
    if (!error) {
        CHECK(regex.re_nsub == 2, "ERE subexpression count %lu", (unsigned long)regex.re_nsub);
        error = regexec(&regex, "ab123", 4, match, 0);
        CHECK(error == 0 && match[0].rm_so == 0 && match[0].rm_eo == 5 &&
              match[1].rm_so == 0 && match[1].rm_eo == 2 &&
              match[2].rm_so == 2 && match[2].rm_eo == 5,
              "ERE captures: %d [%ld,%ld] [%ld,%ld] [%ld,%ld]", error,
              match[0].rm_so, match[0].rm_eo, match[1].rm_so, match[1].rm_eo,
              match[2].rm_so, match[2].rm_eo);
        regfree(&regex);
    }

    CHECK(regcomp(&regex, "a|ab", REG_EXTENDED) == 0, "compile alternation");
    CHECK(regexec(&regex, "zab", 1, match, 0) == 0 &&
          match[0].rm_so == 1 && match[0].rm_eo == 3,
          "leftmost-longest alternation [%ld,%ld]", match[0].rm_so, match[0].rm_eo);
    regfree(&regex);

    CHECK(regcomp(&regex, "^[a-z]+$", REG_EXTENDED | REG_ICASE) == 0,
          "compile case-insensitive class");
    CHECK(regexec(&regex, "MiniOS", 0, 0, 0) == 0, "case-insensitive class match");
    regfree(&regex);

    CHECK(regcomp(&regex, "^b$", REG_EXTENDED | REG_NEWLINE) == 0,
          "compile newline anchors");
    CHECK(regexec(&regex, "a\nb\nc", 1, match, 0) == 0 &&
          match[0].rm_so == 2 && match[0].rm_eo == 3,
          "newline anchors [%ld,%ld]", match[0].rm_so, match[0].rm_eo);
    regfree(&regex);

    CHECK(regcomp(&regex, "^\\(ab*\\)x\\1$", 0) == 0, "compile BRE back reference");
    CHECK(regexec(&regex, "abbbxabbb", 2, match, 0) == 0 &&
          match[1].rm_so == 0 && match[1].rm_eo == 4,
          "BRE back reference");
    regfree(&regex);

    CHECK(regcomp(&regex, "b+", REG_EXTENDED) == 0, "compile STARTEND pattern");
    match[0].rm_so = 3;
    match[0].rm_eo = 6;
    CHECK(regexec(&regex, "aaabbbccc", 1, match, REG_STARTEND) == 0 &&
          match[0].rm_so == 3 && match[0].rm_eo == 6, "REG_STARTEND");
    regfree(&regex);

    error = regcomp(&regex, "[z-a]", REG_EXTENDED);
    char message[64];
    CHECK(error == REG_ERANGE && regerror(error, &regex, message, sizeof message) > 1,
          "invalid range error %d: %s", error, message);
}

static void test_time_additions(void)
{
    struct timespec now, resolution;
    CHECK(timespec_get(&now, TIME_UTC) == TIME_UTC && now.tv_sec > 0,
          "timespec_get");
    CHECK(timespec_getres(&resolution, TIME_UTC) == TIME_UTC &&
          resolution.tv_sec == 0 && resolution.tv_nsec > 0,
          "timespec_getres");
    CHECK(timespec_get(&now, 99) == 0, "unknown time base rejected");
    tzset();
    CHECK(strcmp(tzname[0], "UTC") == 0 && timezone == 0 && daylight == 0,
          "UTC timezone state");
}

int main(void)
{
    test_setjmp();
    test_locale();
    test_wchar();
    test_regex();
    test_time_additions();
    printf("libcexttest: %d failures\n", failures);
    return failures ? 1 : 0;
}
