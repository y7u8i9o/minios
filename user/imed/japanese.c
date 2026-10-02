/* The Japanese engine of imed (I4, docs/design/ime.md), with the keys of
 * Mozc and MS-IME.  Romaji become hiragana in the preedit.  Space or
 * Henkan converts the reading into segments with jpcore.c.  The auxiliary
 * line shows the segments with the current one in brackets.  A second
 * Space opens the candidates of the current segment.
 *
 *   composing:   letters compose, Space converts, Enter commits the kana,
 *                Backspace deletes, Escape drops, F6 to F10 change the form
 *                (hiragana, katakana, half-width katakana, full-width and
 *                half-width letters), Muhenkan toggles katakana
 *   converting:  Space and Down the next candidate, Up the previous one,
 *                1 to 9 choose on the page, Left and Right move between
 *                segments, Shift+Left and Shift+Right resize the current
 *                one, F6 to F8 its form, Enter commits, Escape and
 *                Backspace return to the kana, a letter commits and
 *                composes anew
 *
 * The state below belongs to the single thread of imed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <gui/keymap.h>
#include <minios/input.h>
#include "imed.h"
#include "jpcore.h"
#include "romaji.h"

#define MAX_SEGMENTS 32
#define MAX_CANDS 64

static int loaded;
static char kana[JP_MAX_CHARS * 3 + 1];     /* the reading */
static char roma[8];                        /* romaji not yet converted */
static char letters[JP_MAX_CHARS + 1];      /* the letters as typed, for F9 and F10 */
static int form;                            /* 0, or the function key of the form of the kana */
static int converting, shown, current;
static struct jp_segment segs[MAX_SEGMENTS];
static int nsegs, changed[MAX_SEGMENTS];
static struct jp_cand cands[MAX_CANDS];
static int ncands;

static void load(void)
{
    if (loaded)
        return;
    loaded = 1;
    int err = jp_load("/usr/share/ime/japanese.dict");
    if (err < 0)
        printf("imed: cannot load the Japanese dictionary: %d\n", err);
    const char *home = getenv("HOME");
    char path[256];
    snprintf(path, sizeof path, "%s/.config", home && home[0] == '/' ? home : "/home");
    mkdir(path, 0755);
    strlcat(path, "/imed", sizeof path);
    mkdir(path, 0755);
    strlcat(path, "/japanese.user", sizeof path);
    jp_user_load(path);
}

static int composing(void)
{
    return kana[0] || roma[0];
}

/* in_form writes the composition in the form of F6 to F10. */
static void in_form(int f, char *out, size_t size)
{
    char all[sizeof kana + sizeof roma];
    snprintf(all, sizeof all, "%s%s", kana, roma);
    switch (f) {
    case 7: jp_katakana(all, out, size); break;
    case 8: jp_halfwidth(all, out, size); break;
    case 9: jp_fullwidth(letters, out, size); break;
    case 10: strlcpy(out, letters, size); break;
    default: strlcpy(out, all, size); break;
    }
}

static void clear_table(void)
{
    imed_table.n = 0;
    imed_table.cursor = 0;
    imed_table.aux[0] = '\0';
    imed_table_changed();
}

static void show_compose(void)
{
    char text[512];
    in_form(form, text, sizeof text);
    imed_preedit(text, -1);
    clear_table();
}

/* load_cands fills the candidates of the current segment, with its
 * surface under the cursor. */
static void load_cands(void)
{
    struct jp_segment *s = &segs[current];
    ncands = jp_candidates(kana, s->start, s->end, cands, MAX_CANDS);
    int at = -1;
    for (int i = 0; i < ncands; i++)
        if (strcmp(cands[i].surface, s->surface) == 0)
            at = i;
    if (at < 0 && ncands < MAX_CANDS) {
        memmove(cands + 1, cands, (size_t)ncands * sizeof cands[0]);
        strlcpy(cands[0].surface, s->surface, sizeof cands[0].surface);
        cands[0].lclass = s->lclass;
        cands[0].rclass = s->rclass;
        ncands++;
        at = 0;
    }
    imed_table.cursor = at < 0 ? 0 : at;
}

static void show_convert(void)
{
    char text[512] = "", aux[256] = "";
    int cursor = 0;
    for (int i = 0; i < nsegs; i++) {
        strlcat(text, segs[i].surface, sizeof text);
        if (i == current)
            cursor = (int)strlen(text);
        strlcat(aux, i == current ? "【" : "", sizeof aux);
        strlcat(aux, segs[i].surface, sizeof aux);
        strlcat(aux, i == current ? "】" : "", sizeof aux);
    }
    imed_preedit(text, cursor);
    struct imed_table *t = &imed_table;
    strlcpy(t->aux, aux, sizeof t->aux);
    t->n = shown ? ncands : 0;
    for (int i = 0; i < t->n; i++) {
        strlcpy(t->text[i], cands[i].surface, sizeof t->text[i]);
        t->comment[i][0] = '\0';
    }
    imed_table_changed();
}

static void start_convert(void)
{
    romaji_flush(kana, sizeof kana, roma);
    form = 0;
    nsegs = jp_convert(kana, 0, 0, segs, MAX_SEGMENTS);
    if (!nsegs)
        return;
    memset(changed, 0, sizeof changed);
    converting = 1;
    shown = 0;
    current = 0;
    load_cands();
    show_convert();
}

static void clear(void)
{
    kana[0] = roma[0] = letters[0] = '\0';
    form = 0;
    converting = shown = 0;
    nsegs = 0;
    show_compose();
}

/* commit_convert commits the segments and learns the changed ones. */
static void commit_convert(void)
{
    char text[512] = "";
    for (int i = 0; i < nsegs; i++) {
        strlcat(text, segs[i].surface, sizeof text);
        if (changed[i]) {
            char reading[JP_SURFACE];
            int len = segs[i].end - segs[i].start;
            if (len > 0 && (size_t)len < sizeof reading) {
                memcpy(reading, kana + segs[i].start, (size_t)len);
                reading[len] = '\0';
                jp_user_learn(reading, segs[i].surface, segs[i].lclass, segs[i].rclass);
            }
        }
    }
    imed_commit(text);
    clear();
}

static void commit_compose(void)
{
    char text[512];
    romaji_flush(kana, sizeof kana, roma);
    in_form(form, text, sizeof text);
    imed_commit(text);
    clear();
}

/* choose puts candidate i in the current segment. */
static void choose(int i)
{
    if (i < 0 || i >= ncands)
        return;
    struct jp_segment *s = &segs[current];
    strlcpy(s->surface, cands[i].surface, sizeof s->surface);
    s->lclass = cands[i].lclass;
    s->rclass = cands[i].rclass;
    changed[current] = 1;
    imed_table.cursor = i;
}

static void move_segment(int d)
{
    int next = current + d;
    if (next < 0 || next >= nsegs)
        return;
    current = next;
    shown = 0;
    load_cands();
}

/* resize moves the end of the current segment by one character and
 * converts the rest again. */
static void resize(int d)
{
    struct jp_segment *s = &segs[current];
    int end = s->end, len = (int)strlen(kana);
    if (d > 0) {
        if (end >= len)
            return;
        unsigned char c = (unsigned char)kana[end];
        end += c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
    } else {
        int p = end - 1;
        while (p > s->start && ((unsigned char)kana[p] & 0xc0) == 0x80)
            p--;
        if (p <= s->start)
            return;
        end = p;
    }
    int start = s->start;
    int n = jp_convert(kana, start, end < len ? end : 0, segs + current, MAX_SEGMENTS - current);
    nsegs = current + n;
    for (int i = current; i < nsegs; i++)
        changed[i] = 0;
    shown = 0;
    load_cands();
}

/* form_of_segment puts the reading of the current segment in a form. */
static void form_of_segment(int f)
{
    struct jp_segment *s = &segs[current];
    char reading[JP_SURFACE];
    int len = s->end - s->start;
    if (len <= 0 || (size_t)len >= sizeof reading)
        return;
    memcpy(reading, kana + s->start, (size_t)len);
    reading[len] = '\0';
    if (f == 7)
        jp_katakana(reading, s->surface, sizeof s->surface);
    else if (f == 8)
        jp_halfwidth(reading, s->surface, sizeof s->surface);
    else
        strlcpy(s->surface, reading, sizeof s->surface);
    changed[current] = 1;
    load_cands();
}

static int compose_key(uint32_t key, int ch);

static int convert_key(uint32_t key, int ch, int mods)
{
    int shift = mods & KEYMAP_MOD_SHIFT;
    if (key == KEY_SPACE || key == KEY_HENKAN || key == KEY_DOWN) {
        if (!shown)
            shown = 1;
        choose((imed_table.cursor + 1) % ncands);
    } else if (key == KEY_UP) {
        shown = 1;
        choose((imed_table.cursor + ncands - 1) % ncands);
    } else if (shown && ch >= '1' && ch <= '9') {
        int i = imed_page_first() + ch - '1';
        if (i < ncands && i < imed_page_first() + imed_table.page_size) {
            choose(i);
            shown = 0;
            move_segment(1);
        }
    } else if (key == KEY_LEFT || key == KEY_RIGHT) {
        if (shift)
            resize(key == KEY_RIGHT ? 1 : -1);
        else
            move_segment(key == KEY_RIGHT ? 1 : -1);
    } else if (key == KEY_ENTER || key == KEY_KPENTER) {
        commit_convert();
        return 1;
    } else if (key == KEY_ESC || key == KEY_BACKSPACE) {
        converting = shown = 0;
        nsegs = 0;
        show_compose();
        return 1;
    } else if (key >= KEY_F6 && key <= KEY_F8) {
        form_of_segment(key - KEY_F6 + 6);
    } else if (ch > ' ' && ch < 127) {
        commit_convert();
        return compose_key(key, ch);
    }
    show_convert();
    return 1;
}

static int compose_key(uint32_t key, int ch)
{
    int letter = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch && strchr("-,.[]'", ch));
    if (ch > ' ' && ch < 127 && (letter || composing())) {
        size_t n = strlen(letters);
        if (n + 1 < sizeof letters) {
            letters[n] = (char)ch;
            letters[n + 1] = '\0';
        }
        if (form >= 9)
            form = 0;
        romaji_feed(kana, sizeof kana, roma, sizeof roma, (char)ch);
        show_compose();
        return 1;
    }
    if (!composing())
        return 0;
    if (key == KEY_SPACE || key == KEY_HENKAN) {
        start_convert();
        return 1;
    }
    if (key == KEY_ENTER || key == KEY_KPENTER) {
        commit_compose();
    } else if (key == KEY_BACKSPACE) {
        size_t n = strlen(roma);
        if (n) {
            roma[n - 1] = '\0';
        } else {
            n = strlen(kana);
            while (n > 0 && ((unsigned char)kana[--n] & 0xc0) == 0x80)
                ;
            kana[n] = '\0';
        }
        size_t l = strlen(letters);
        if (l)
            letters[l - 1] = '\0';
        if (!composing())
            letters[0] = '\0';
        show_compose();
    } else if (key == KEY_ESC) {
        clear();
    } else if (key >= KEY_F6 && key <= KEY_F10) {
        romaji_flush(kana, sizeof kana, roma);
        form = (int)(key - KEY_F6) + 6;
        show_compose();
    } else if (key == KEY_MUHENKAN) {
        form = form == 7 ? 0 : 7;
        show_compose();
    }
    return 1;
}

static int japanese_key(uint32_t key, int ch, int mods)
{
    load();
    if (converting)
        return convert_key(key, ch, mods);
    return compose_key(key, ch);
}

static void japanese_flush(void)
{
    if (converting)
        commit_convert();
    else if (composing())
        commit_compose();
}

static void japanese_select(void)
{
    imed_set_label("あ");
}

static void japanese_reset(void)
{
    kana[0] = roma[0] = letters[0] = '\0';
    form = converting = shown = nsegs = 0;
    imed_table.n = 0;
    imed_table.aux[0] = '\0';
}

static void japanese_clicked(int index)
{
    if (!converting)
        return;
    choose(index);
    shown = 0;
    move_segment(1);
    show_convert();
}

const struct imed_engine japanese_engine = {
    "japanese", "あ", "Japanese (Mozc dictionary)", load, japanese_select, japanese_key, japanese_flush,
    japanese_reset, japanese_clicked,
};
