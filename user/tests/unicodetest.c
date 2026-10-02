/* L0: the character classes, case mappings and widths that libc takes from
 * the Unicode Character Database, and the cells of the terminal emulator
 * for wide characters and combining marks.  The terminal emulator has no
 * drawing code, and its source is compiled into this program. */
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include "../term/vt.c"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("unicodetest: FAIL " __VA_ARGS__); printf("\n"); } } while (0)

static void test_widths(void)
{
    static const struct { unsigned cp; int width; } cases[] = {
        { 'A', 1 }, { 0x301, 0 }, { 0x4e00, 2 }, { 0x3042, 2 }, { 0x30a2, 2 }, { 0xff21, 2 },
        { 0x1f600, 2 }, { 0x200b, 0 }, { 0xad, 1 }, { 0x378, -1 }, { 0x1100, 2 }, { 0x1160, 0 },
        { 0xac00, 2 }, { 0x416, 1 }, { 0x3b1, 1 }, { 0x7f, -1 }, { 0x20000, 2 }, { 0xe0041, 0 },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++)
        CHECK(wcwidth((wchar_t)cases[i].cp) == cases[i].width, "wcwidth(U+%04X) is %d, expected %d", cases[i].cp,
              wcwidth((wchar_t)cases[i].cp), cases[i].width);
    CHECK(wcswidth(L"a中́b", 4) == 4, "wcswidth of a mixed string");
}

static void test_classes(void)
{
    static const unsigned letters[] = { 0x4e2d, 0x3042, 0x30a2, 0x3b1, 0x416, 0x5d0, 0x627, 0xac00, 0x1e9e };
    for (size_t i = 0; i < sizeof letters / sizeof letters[0]; i++)
        CHECK(iswalpha(letters[i]) && iswprint(letters[i]) && iswgraph(letters[i]) && !iswpunct(letters[i]),
              "U+%04X is a printable letter", letters[i]);
    CHECK(!iswalpha(0x660) && !iswdigit(0x660) && !iswalpha('_') && !iswalpha(0x3002), "digits and punctuation are not letters");
    CHECK(iswupper(0x391) && iswlower(0x3b1) && iswupper(0x416) && iswlower(0x436), "Greek and Cyrillic case");
    CHECK(!iswupper(0x4e2d) && !iswlower(0x4e2d), "Han characters have no case");
    CHECK(iswpunct('!') && iswpunct(0x3002) && iswpunct(0x20ac) && iswpunct(0xff01) && !iswpunct('a'),
          "punctuation and symbols");
    CHECK(!iswprint(0x378) && iswprint(0xe000) && !iswprint(0xd800), "unassigned, private use and surrogates");
    CHECK(iswspace(0x3000) && !iswspace(0xa0) && iswprint(0xa0) && !iswgraph(0xa0), "spaces");
}

static void test_case(void)
{
    static const struct { unsigned from, upper, lower; } cases[] = {
        { 0x3b1, 0x391, 0x3b1 }, { 0x3c2, 0x3a3, 0x3c2 }, { 0x436, 0x416, 0x436 }, { 0x101, 0x100, 0x101 },
        { 0x17f, 'S', 0x17f }, { 0xff41, 0xff21, 0xff41 }, { 0x1f3, 0x1f1, 0x1f3 }, { 0x130, 0x130, 'i' },
        { 0x1e9e, 0x1e9e, 0xdf }, { 0xdf, 0xdf, 0xdf }, { 0x4e2d, 0x4e2d, 0x4e2d }, { 0x10428, 0x10400, 0x10428 },
        { 0x450, 0x400, 0x450 }, { 0x1f80, 0x1f88, 0x1f80 },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        CHECK(towupper(cases[i].from) == cases[i].upper, "towupper(U+%04X) is U+%04X", cases[i].from,
              (unsigned)towupper(cases[i].from));
        CHECK(towlower(cases[i].from) == cases[i].lower, "towlower(U+%04X) is U+%04X", cases[i].from,
              (unsigned)towlower(cases[i].from));
    }
}

static const struct vcell *at(struct vt *v, int row, int col)
{
    return &v->cells[row * v->cols + col];
}

static void feed(struct vt *v, const char *s)
{
    vt_feed(v, s, strlen(s));
}

static void test_terminal(void)
{
    struct vt *v = vt_create(10, 3, 0);
    feed(v, "a\xe4\xb8\xad" "b");
    CHECK(at(v, 0, 0)->cp == 'a' && at(v, 0, 1)->cp == 0x4e2d && at(v, 0, 2)->cp == VC_WIDE_TAIL &&
          at(v, 0, 3)->cp == 'b' && v->cx == 4, "a wide character occupies two cells: cursor at %d", v->cx);
    feed(v, "e\xcc\x81");
    CHECK(at(v, 0, 4)->cp == 'e' && at(v, 0, 4)->mark == 0x301 && v->cx == 5, "a combining mark attaches to its base");
    feed(v, "\x1b[1;2Hx");
    CHECK(at(v, 0, 1)->cp == 'x' && at(v, 0, 2)->cp == ' ', "overwriting the left half clears the right half");
    feed(v, "\x1b[1;4H\xe4\xb8\xad\x1b[1;5Hy");
    CHECK(at(v, 0, 3)->cp == ' ' && at(v, 0, 4)->cp == 'y', "overwriting the right half clears the left half");
    vt_free(v);

    v = vt_create(4, 3, 0);
    feed(v, "abc\xe4\xb8\xad");
    CHECK(at(v, 0, 3)->cp == ' ' && at(v, 1, 0)->cp == 0x4e2d && at(v, 1, 1)->cp == VC_WIDE_TAIL && v->cy == 1 &&
          v->cx == 2, "a wide character in the last column starts a new line: row %d column %d", v->cy, v->cx);
    feed(v, "d\xe4\xb8\xad");
    CHECK(at(v, 1, 2)->cp == 'd' && at(v, 1, 3)->cp == ' ' && at(v, 2, 0)->cp == 0x4e2d,
          "a wide character after the third column wraps");
    vt_free(v);
}

int main(void)
{
    test_widths();
    test_classes();
    test_case();
    test_terminal();
    printf("unicodetest: %d failures\n", failures);
    return failures != 0;
}
