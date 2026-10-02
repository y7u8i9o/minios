/* The test engine of imed (imed -t), for the boot tests of the protocol and
 * the candidate window.  Letters collect in the preedit.  The candidates
 * are the letters in capitals, in small letters and with a capital first.
 * Space chooses the candidate under the cursor, 1 to 9 choose by number on
 * the page, Enter commits the letters, Escape drops them, and the wheel
 * and the arrows of the window turn the pages.  F12 replies after 300 ms,
 * after the timeout of the compositor. */
#include <ctype.h>
#include <string.h>
#include <unistd.h>
#include <gui/keymap.h>
#include <minios/input.h>
#include "imed.h"

static char letters[64];

static void show(void)
{
    imed_preedit(letters, -1);
    struct imed_table *t = &imed_table;
    t->n = 0;
    t->cursor = 0;
    t->aux[0] = '\0';
    if (letters[0]) {
        size_t n = strlen(letters);
        for (int k = 0; k < 3; k++) {
            for (size_t i = 0; i <= n; i++) {
                char c = letters[i];
                t->text[k][i] = (char)(k == 0 || (k == 2 && i == 0) ? toupper((unsigned char)c) : tolower((unsigned char)c));
            }
            strlcpy(t->comment[k], k == 0 ? "upper" : k == 1 ? "lower" : "title", sizeof t->comment[k]);
        }
        t->n = 3;
        strlcpy(t->aux, letters, sizeof t->aux);
    }
    imed_table_changed();
}

static void choose(int i)
{
    if (i < 0 || i >= imed_table.n)
        return;
    imed_commit(imed_table.text[i]);
    letters[0] = '\0';
    show();
}

static void test_select(void)
{
    imed_set_label("T");
}

static int test_key(uint32_t key, int ch, int mods)
{
    if (key == KEY_F12) {
        usleep(300000);
        return 1;
    }
    size_t n = strlen(letters);
    if (isalpha(ch) && n + 1 < sizeof letters) {
        letters[n] = (char)ch;
        letters[n + 1] = '\0';
        show();
        return 1;
    }
    if (!n)
        return 0;
    if (key == KEY_SPACE)
        choose(imed_table.cursor);
    else if (ch >= '1' && ch <= '9')
        choose(imed_page_first() + ch - '1');
    else if (key == KEY_ENTER) {
        imed_commit(letters);
        letters[0] = '\0';
        show();
    } else if (key == KEY_ESC) {
        letters[0] = '\0';
        show();
    } else if (key == KEY_BACKSPACE) {
        letters[n - 1] = '\0';
        show();
    } else if (key == KEY_RIGHT || key == KEY_LEFT) {
        imed_table.cursor = (imed_table.cursor + (key == KEY_RIGHT ? 1 : imed_table.n - 1)) % imed_table.n;
        imed_table_changed();
    }
    return 1;
}

static void test_flush(void)
{
    if (letters[0]) {
        imed_commit(letters);
        letters[0] = '\0';
        show();
    }
}

static void test_reset(void)
{
    letters[0] = '\0';
    imed_table.n = 0;
}

const struct imed_engine test_engine = {
    "test", "T", "Test engine", NULL, test_select, test_key, test_flush, test_reset, choose,
};
