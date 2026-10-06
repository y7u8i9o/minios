/* painter_wrap, which the panel uses for the bodies of notifications
 * (docs/design/notifications.md). The widths are measured with the
 * default font, so the tests do not depend on its metrics. */
#include <gui/paint.h>
#include <stdlib.h>
#include <string.h>
#include "check.h"

static int line_is(const char *text, const int *start, const int *len, int i, const char *expected)
{
    return len[i] == (int)strlen(expected) && memcmp(text + start[i], expected, (size_t)len[i]) == 0;
}

void run_wrap_tests(void)
{
    struct surface s = { calloc(4 * 4, 4), 4, 4, 4 };
    struct theme t;
    theme_init_default(&t);
    struct painter p;
    painter_init(&p, &s, &t);
    int start[8], len[8];

    const char *words = "hello world foo";
    int n = painter_wrap(&p, words, painter_text_width(&p, "hello world", -1), start, len, 8);
    CHECK(n == 2 && line_is(words, start, len, 0, "hello world") && line_is(words, start, len, 1, "foo"),
          "wrapping at a space: %d lines", n);

    const char *word = "abcdefghij";
    n = painter_wrap(&p, word, painter_text_width(&p, "abcde", -1), start, len, 8);
    CHECK(n == 2 && line_is(word, start, len, 0, "abcde") && line_is(word, start, len, 1, "fghij"),
          "a long word breaks between characters: %d lines", n);

    const char *lines = "one\ntwo";
    n = painter_wrap(&p, lines, 1000, start, len, 8);
    CHECK(n == 2 && line_is(lines, start, len, 0, "one") && line_is(lines, start, len, 1, "two"),
          "a newline ends a line: %d lines", n);

    CHECK(painter_wrap(&p, "", 100, start, len, 8) == 0, "empty text has no lines");

    n = painter_wrap(&p, "a b c d", painter_text_width(&p, "a", -1), start, len, 2);
    CHECK(n == 4 && line_is("a b c d", start, len, 1, "b"), "the count includes lines beyond max: %d", n);

    const char *umlauts = "\xc3\xa4\xc3\xb6\xc3\xbc";
    n = painter_wrap(&p, umlauts, painter_text_width(&p, "\xc3\xa4", -1), start, len, 8);
    CHECK(n == 3 && len[0] == 2 && len[1] == 2 && len[2] == 2, "characters are never split: %d lines", n);

    n = painter_wrap(&p, "xyz", 0, start, len, 8);
    CHECK(n == 3, "a width below one character still makes progress: %d lines", n);
    free(s.pixels);
}
