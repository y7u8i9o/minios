/* The pinyin engine of imed (I3, docs/design/ime.md), with the keys of
 * Rime.  Letters and apostrophes collect in the input.  The candidates of
 * pycore.c fill the lookup table and the auxiliary line shows the letters
 * divided into syllables.  A candidate that covers only the first letters
 * is fixed in the preedit and the rest of the input continues.  When the
 * input is used up, the fixed words are committed and learned, and a
 * phrase of several words is learned as well.
 *
 *   letters, '         compose
 *   Space, 1 to 9      choose the cursor candidate, or one of the page
 *   arrows             move the cursor
 *   - = , . PageUp PageDown   turn the pages
 *   Enter              commit the letters as they are
 *   Escape             drop the input
 *   Backspace          delete a letter, or undo the last fixed word
 *   punctuation        the Chinese punctuation, after the sentence
 *
 * The state below belongs to the single thread of imed. */
#include <minios/conf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <gui/keymap.h>
#include <minios/input.h>
#include "imed.h"
#include "pycore.h"

#define MAX_FIXED 16

struct fixed {
    char text[96];
    char letters[PY_MAX_LETTERS + 1];
    uint16_t ids[PY_MAX_SENTENCE_SYLLABLES];
    int nids;
};

static int loaded;
static char input[PY_MAX_LETTERS + 1];
static struct fixed fixed[MAX_FIXED];
static int nfixed;
static struct py_cand cands[IMED_MAX_CANDIDATES];
static int ncands;
static int double_quote_open, single_quote_open;

static void load(void)
{
    if (loaded)
        return;
    loaded = 1;
    int err = py_load("/usr/share/ime/pinyin.dict");
    if (err < 0)
        printf("imed: cannot load the pinyin dictionary: %d\n", err);
    const char *home = getenv("HOME");
    char path[256];
    snprintf(path, sizeof path, "%s/.config", home && home[0] == '/' ? home : conf_home());
    mkdir(path, 0755);
    strlcat(path, "/imed", sizeof path);
    mkdir(path, 0755);
    strlcat(path, "/pinyin.user", sizeof path);
    py_user_load(path);
}

static int composing(void)
{
    return input[0] || nfixed;
}

/* show computes the candidates of the input and shows the preedit, the
 * candidates and the syllables. */
static void show(void)
{
    char preedit[512] = "";
    for (int i = 0; i < nfixed; i++)
        strlcat(preedit, fixed[i].text, sizeof preedit);
    strlcat(preedit, input, sizeof preedit);
    imed_preedit(preedit, -1);
    struct imed_table *t = &imed_table;
    ncands = input[0] ? py_candidates(input, cands, IMED_MAX_CANDIDATES, t->aux, sizeof t->aux) : 0;
    t->n = ncands;
    t->cursor = 0;
    for (int i = 0; i < ncands; i++) {
        strlcpy(t->text[i], cands[i].text, sizeof t->text[i]);
        t->comment[i][0] = '\0';
    }
    if (!input[0])
        t->aux[0] = '\0';
    imed_table_changed();
}

static void clear(void)
{
    input[0] = '\0';
    nfixed = 0;
    show();
}

/* learn records the fixed words, and the phrase that they form when there
 * are several. */
static void learn(void)
{
    uint16_t ids[PY_MAX_WORD_SYLLABLES];
    int nids = 0, fits = 1;
    char phrase[96] = "";
    for (int i = 0; i < nfixed; i++) {
        if (fixed[i].nids <= PY_MAX_WORD_SYLLABLES)
            py_user_learn(fixed[i].text, fixed[i].ids, fixed[i].nids);
        for (int k = 0; k < fixed[i].nids; k++)
            if (nids < PY_MAX_WORD_SYLLABLES)
                ids[nids++] = fixed[i].ids[k];
            else
                fits = 0;
        strlcat(phrase, fixed[i].text, sizeof phrase);
    }
    if (nfixed > 1 && fits)
        py_user_learn(phrase, ids, nids);
}

static void commit_fixed(void)
{
    char text[512] = "";
    for (int i = 0; i < nfixed; i++)
        strlcat(text, fixed[i].text, sizeof text);
    imed_commit(text);
}

/* choose fixes candidate i.  When it uses up the input, everything is
 * committed. */
static void choose(int i)
{
    if (i < 0 || i >= ncands || nfixed == MAX_FIXED)
        return;
    struct fixed *f = &fixed[nfixed++];
    strlcpy(f->text, cands[i].text, sizeof f->text);
    int end = cands[i].end;
    memcpy(f->letters, input, (size_t)end);
    f->letters[end] = '\0';
    memcpy(f->ids, cands[i].ids, (size_t)cands[i].nids * sizeof f->ids[0]);
    f->nids = cands[i].nids;
    memmove(input, input + end, strlen(input + end) + 1);
    while (input[0] == '\'')
        memmove(input, input + 1, strlen(input));
    if (!input[0]) {
        commit_fixed();
        learn();
        nfixed = 0;
    }
    show();
}

/* commit_best commits the input with the first candidate at each step:
 * the sentence, or the longest word.  Letters without a candidate are
 * committed as they are. */
static void flush(void);

static void commit_best(void)
{
    for (int guard = 0; input[0] && ncands && guard < PY_MAX_LETTERS; guard++)
        choose(0);
    flush();
}

/* flush commits the fixed words and the letters as they are. */
static void flush(void)
{
    if (!composing())
        return;
    commit_fixed();
    imed_commit(input);
    clear();
}

static const char *punctuation(int ch)
{
    switch (ch) {
    case ',': return "，";
    case '.': return "。";
    case '?': return "？";
    case '!': return "！";
    case ';': return "；";
    case ':': return "：";
    case '(': return "（";
    case ')': return "）";
    case '[': return "【";
    case ']': return "】";
    case '<': return "《";
    case '>': return "》";
    case '\\': return "、";
    case '^': return "……";
    case '_': return "——";
    case '~': return "～";
    case '$': return "￥";
    case '"': return (double_quote_open = !double_quote_open) ? "“" : "”";
    case '\'': return (single_quote_open = !single_quote_open) ? "‘" : "’";
    }
    return NULL;
}

static void page(int pages)
{
    window_scroll(pages);
}

static void move(int d)
{
    if (!ncands)
        return;
    imed_table.cursor = (imed_table.cursor + d + ncands) % ncands;
    imed_table_changed();
}

static int pinyin_key(uint32_t key, int ch, int mods)
{
    load();
    size_t n = strlen(input);
    if ((ch >= 'a' && ch <= 'z') || (ch == '\'' && input[0])) {
        if (n + 1 < sizeof input && n + 1 < PY_MAX_LETTERS) {
            input[n] = (char)ch;
            input[n + 1] = '\0';
        }
        show();
        return 1;
    }
    if (!composing()) {
        const char *p = punctuation(ch);
        if (p) {
            imed_commit(p);
            return 1;
        }
        return 0;
    }
    if (key == KEY_SPACE) {
        if (ncands)
            choose(imed_table.cursor);
        else
            flush();
    } else if (ch >= '1' && ch <= '9') {
        int i = imed_page_first() + ch - '1';
        if (i < ncands && i < imed_page_first() + imed_table.page_size)
            choose(i);
    } else if (key == KEY_ENTER || key == KEY_KPENTER) {
        flush();
    } else if (key == KEY_ESC) {
        clear();
    } else if (key == KEY_BACKSPACE) {
        if (n) {
            input[n - 1] = '\0';
        } else if (nfixed) {
            nfixed--;
            strlcpy(input, fixed[nfixed].letters, sizeof input);
        }
        show();
    } else if (key == KEY_LEFT || key == KEY_UP) {
        move(-1);
    } else if (key == KEY_RIGHT || key == KEY_DOWN) {
        move(1);
    } else if (ch == '-' || ch == ',' || key == KEY_PAGEUP) {
        page(-1);
    } else if (ch == '=' || ch == '.' || key == KEY_PAGEDOWN) {
        page(1);
    } else if (ch > ' ' && ch < 127) {
        /* Other punctuation ends the input with the sentence. */
        const char *p = punctuation(ch);
        commit_best();
        if (p)
            imed_commit(p);
        else
            return 0;
    }
    return 1;
}

static void pinyin_select(void)
{
    load();
    imed_set_label("拼");
}

static void pinyin_reset(void)
{
    input[0] = '\0';
    nfixed = 0;
    ncands = 0;
    imed_table.n = 0;
    imed_table.aux[0] = '\0';
}

static void pinyin_clicked(int index)
{
    choose(index);
}

const struct imed_engine pinyin_engine = {
    "pinyin", "拼", "Chinese (Pinyin)", load, pinyin_select, pinyin_key, flush, pinyin_reset, pinyin_clicked,
};
