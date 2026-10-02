/* The built-in Japanese input method (L6, docs/design/ime.md), until the
 * Japanese engine of imed replaces it (I4).  It converts romaji to
 * hiragana and offers the kanji of the longest reading at the start of the
 * text as candidates.  The engine knows nothing of the protocol: ime_key
 * returns the text to commit and the new preedit, and text.c sends them.  The candidates are drawn by the compositor over every surface,
 * below the caret that the client reported.
 *
 * The state below belongs to the single thread of the compositor. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <gui/keymap.h>
#include <gui/paint.h>
#include <gui/utf8.h>
#include <minios/input.h>
#include "comp.h"

#define MAX_TEXT   64           /* bytes of kana */
#define MAX_CANDS  96
#define CAND_BYTES (MAX_TEXT + 8)
#define PAGE       9
#define PAD        6

/* A candidate table: lines "reading\tcharacters", sorted by reading in
 * code point order, which is also the byte order of UTF-8. */
struct table {
    char *data;
    char **keys;                /* the readings; the characters follow the '\0' */
    int n;
    int tried;
};

static struct table kana_table;
static int mode;                /* IME_OFF or IME_JAPANESE */
static char text[MAX_TEXT + 1]; /* hiragana */
static char roma[8];            /* romaji not yet converted to kana */
static int katakana;            /* F7 shows the kana as katakana */
static char cands[MAX_CANDS][CAND_BYTES];
static int ncands, sel;
static size_t cand_cover;       /* bytes of text that a candidate replaces */
static int anchor_x, anchor_y, anchor_h;
static struct rect shown_box;   /* the candidate box on the screen, or empty */

/* ---- tables ---- */

static void table_load(struct table *t, const char *path)
{
    if (t->tried)
        return;
    t->tried = 1;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        comp_log("ime: cannot open %s", path);
        return;
    }
    off_t size = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    t->data = size > 0 ? malloc((size_t)size + 1) : NULL;
    if (!t->data || read(fd, t->data, (size_t)size) != size) {
        close(fd);
        free(t->data);
        t->data = NULL;
        return;
    }
    close(fd);
    t->data[size] = '\0';
    int lines = 0;
    for (char *p = t->data; *p; p++)
        lines += *p == '\n';
    t->keys = malloc(sizeof *t->keys * (size_t)(lines + 1));
    if (!t->keys)
        return;
    for (char *p = t->data; *p;) {
        char *end = strchr(p, '\n');
        if (end)
            *end = '\0';
        char *tab = strchr(p, '\t');
        if (*p != '#' && tab) {
            *tab = '\0';
            t->keys[t->n++] = p;
        }
        if (!end)
            break;
        p = end + 1;
    }
    comp_log("ime: %d readings in %s", t->n, path);
}

/* table_find returns the characters of the reading s[0..len), or NULL. */
static const char *table_find(const struct table *t, const char *s, size_t len)
{
    int lo = 0, hi = t->n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const char *k = t->keys[mid];
        int c = strncmp(k, s, len);
        if (c == 0)
            c = k[len] ? 1 : 0;
        if (c == 0)
            return k + strlen(k) + 1;
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

/* ---- romaji ---- */

static const char *const romaji[][2] = {
    { "a", "あ" }, { "i", "い" }, { "u", "う" }, { "e", "え" }, { "o", "お" },
    { "ka", "か" }, { "ki", "き" }, { "ku", "く" }, { "ke", "け" }, { "ko", "こ" },
    { "sa", "さ" }, { "si", "し" }, { "shi", "し" }, { "su", "す" }, { "se", "せ" }, { "so", "そ" },
    { "ta", "た" }, { "ti", "ち" }, { "chi", "ち" }, { "tu", "つ" }, { "tsu", "つ" }, { "te", "て" }, { "to", "と" },
    { "na", "な" }, { "ni", "に" }, { "nu", "ぬ" }, { "ne", "ね" }, { "no", "の" },
    { "ha", "は" }, { "hi", "ひ" }, { "hu", "ふ" }, { "fu", "ふ" }, { "he", "へ" }, { "ho", "ほ" },
    { "ma", "ま" }, { "mi", "み" }, { "mu", "む" }, { "me", "め" }, { "mo", "も" },
    { "ya", "や" }, { "yu", "ゆ" }, { "yo", "よ" },
    { "ra", "ら" }, { "ri", "り" }, { "ru", "る" }, { "re", "れ" }, { "ro", "ろ" },
    { "wa", "わ" }, { "wo", "を" }, { "nn", "ん" }, { "n'", "ん" },
    { "ga", "が" }, { "gi", "ぎ" }, { "gu", "ぐ" }, { "ge", "げ" }, { "go", "ご" },
    { "za", "ざ" }, { "zi", "じ" }, { "ji", "じ" }, { "zu", "ず" }, { "ze", "ぜ" }, { "zo", "ぞ" },
    { "da", "だ" }, { "di", "ぢ" }, { "du", "づ" }, { "de", "で" }, { "do", "ど" },
    { "ba", "ば" }, { "bi", "び" }, { "bu", "ぶ" }, { "be", "べ" }, { "bo", "ぼ" },
    { "pa", "ぱ" }, { "pi", "ぴ" }, { "pu", "ぷ" }, { "pe", "ぺ" }, { "po", "ぽ" },
    { "kya", "きゃ" }, { "kyu", "きゅ" }, { "kyo", "きょ" },
    { "sha", "しゃ" }, { "shu", "しゅ" }, { "sho", "しょ" }, { "she", "しぇ" },
    { "sya", "しゃ" }, { "syu", "しゅ" }, { "syo", "しょ" },
    { "cha", "ちゃ" }, { "chu", "ちゅ" }, { "cho", "ちょ" }, { "che", "ちぇ" },
    { "tya", "ちゃ" }, { "tyu", "ちゅ" }, { "tyo", "ちょ" },
    { "nya", "にゃ" }, { "nyu", "にゅ" }, { "nyo", "にょ" },
    { "hya", "ひゃ" }, { "hyu", "ひゅ" }, { "hyo", "ひょ" },
    { "mya", "みゃ" }, { "myu", "みゅ" }, { "myo", "みょ" },
    { "rya", "りゃ" }, { "ryu", "りゅ" }, { "ryo", "りょ" },
    { "gya", "ぎゃ" }, { "gyu", "ぎゅ" }, { "gyo", "ぎょ" },
    { "ja", "じゃ" }, { "ju", "じゅ" }, { "jo", "じょ" }, { "je", "じぇ" },
    { "jya", "じゃ" }, { "jyu", "じゅ" }, { "jyo", "じょ" },
    { "zya", "じゃ" }, { "zyu", "じゅ" }, { "zyo", "じょ" },
    { "bya", "びゃ" }, { "byu", "びゅ" }, { "byo", "びょ" },
    { "pya", "ぴゃ" }, { "pyu", "ぴゅ" }, { "pyo", "ぴょ" },
    { "fa", "ふぁ" }, { "fi", "ふぃ" }, { "fe", "ふぇ" }, { "fo", "ふぉ" },
    { "va", "ゔぁ" }, { "vi", "ゔぃ" }, { "vu", "ゔ" }, { "ve", "ゔぇ" }, { "vo", "ゔぉ" },
    { "thi", "てぃ" }, { "dhi", "でぃ" },
    { "xa", "ぁ" }, { "xi", "ぃ" }, { "xu", "ぅ" }, { "xe", "ぇ" }, { "xo", "ぉ" },
    { "la", "ぁ" }, { "li", "ぃ" }, { "lu", "ぅ" }, { "le", "ぇ" }, { "lo", "ぉ" },
    { "xya", "ゃ" }, { "xyu", "ゅ" }, { "xyo", "ょ" }, { "lya", "ゃ" }, { "lyu", "ゅ" }, { "lyo", "ょ" },
    { "xtu", "っ" }, { "ltu", "っ" }, { "xtsu", "っ" },
    { "-", "ー" }, { ",", "、" }, { ".", "。" }, { "[", "「" }, { "]", "」" },
};
#define NROMAJI (sizeof romaji / sizeof romaji[0])

static void text_append(const char *s)
{
    if (strlen(text) + strlen(s) <= MAX_TEXT)
        strcat(text, s);
}

static void roma_drop(size_t n)
{
    memmove(roma, roma + n, strlen(roma + n) + 1);
}

/* roma_convert turns the pending romaji into kana as far as it can.  A
 * sequence that is the start of a longer one waits.  A doubled consonant
 * is a small tsu, an n before a consonant is ん, and a letter that starts
 * no sequence stays as it is. */
static void roma_convert(void)
{
    while (roma[0]) {
        int exact = -1, longer = 0;
        size_t n = strlen(roma);
        for (size_t i = 0; i < NROMAJI; i++) {
            if (strcmp(romaji[i][0], roma) == 0)
                exact = (int)i;
            else if (strncmp(romaji[i][0], roma, n) == 0)
                longer = 1;
        }
        if (exact >= 0 && !longer) {
            text_append(romaji[exact][1]);
            roma[0] = '\0';
            return;
        }
        if (longer)
            return;
        if (n >= 2 && roma[0] == roma[1] && roma[0] != 'n' && !strchr("aiueo", roma[0])) {
            text_append("っ");
        } else if (roma[0] == 'n') {
            text_append("ん");
        } else {
            char one[2] = { roma[0], '\0' };
            text_append(one);
        }
        roma_drop(1);
    }
}

/* roma_flush converts what remains at the end of the input: a single n is
 * ん, and other letters stay as they are. */
static void roma_flush(void)
{
    roma_convert();
    if (strcmp(roma, "n") == 0)
        text_append("ん");
    else
        text_append(roma);
    roma[0] = '\0';
}

/* to_katakana converts the hiragana of s, which have the code points
 * U+3041 to U+3096, to the katakana 0x60 above them. */
static void to_katakana(const char *s, size_t len, char *out, size_t size)
{
    size_t o = 0;
    int at = 0;
    while ((size_t)at < len) {
        uint32_t cp = gui_utf8_decode(s, (int)len, &at);
        if (cp >= 0x3041 && cp <= 0x3096)
            cp += 0x60;
        char buf[5];
        int m = gui_utf8_encode(cp, buf);
        if (o + (size_t)m + 1 > size)
            break;
        memcpy(out + o, buf, (size_t)m);
        o += (size_t)m;
    }
    out[o] = '\0';
}

/* ---- candidates ---- */

static void damage_box(void)
{
    if (!rect_empty(shown_box))
        scene_damage(shown_box);
}

static struct rect box_rect(void);

static void cands_changed(void)
{
    damage_box();
    shown_box = ncands ? box_rect() : (struct rect){ 0, 0, 0, 0 };
    damage_box();
}

static void cands_clear(void)
{
    ncands = 0;
    sel = 0;
    cand_cover = 0;
    cands_changed();
}

static void cand_add(const char *s, size_t len)
{
    if (ncands < MAX_CANDS && len < CAND_BYTES) {
        memcpy(cands[ncands], s, len);
        cands[ncands][len] = '\0';
        ncands++;
    }
}

/* cands_lookup fills the candidates of the longest reading at the start
 * of the text.  Each character of the table is one candidate.  The
 * Japanese engine adds the reading in hiragana and in katakana, and offers
 * the whole text in both forms when no reading matches. */
static void cands_lookup(void)
{
    struct table *t = &kana_table;
    ncands = 0;
    sel = 0;
    size_t len = strlen(text);
    const char *chars = NULL;
    while (len > 0) {
        if (t->n && (chars = table_find(t, text, len)) != NULL)
            break;
        do
            len--;
        while (len > 0 && ((unsigned char)text[len] & 0xc0) == 0x80);
    }
    if (chars) {
        int n = (int)strlen(chars);
        for (int at = 0; at < n;) {
            int start = at;
            gui_utf8_decode(chars, n, &at);
            cand_add(chars + start, (size_t)(at - start));
        }
    } else {
        len = strlen(text);
    }
    if (mode == IME_JAPANESE && len) {
        char kata[CAND_BYTES];
        cand_add(text, len);
        to_katakana(text, len, kata, sizeof kata);
        cand_add(kata, strlen(kata));
    }
    cand_cover = len;
    cands_changed();
}

/* ---- the engines ---- */

static void result_preedit(struct ime_result *r)
{
    r->preedit_changed = 1;
    if (ncands && mode == IME_JAPANESE) {
        snprintf(r->preedit, sizeof r->preedit, "%s%s", cands[sel], text + cand_cover);
    } else if (katakana) {
        to_katakana(text, strlen(text), r->preedit, sizeof r->preedit);
        strlcat(r->preedit, roma, sizeof r->preedit);
    } else {
        snprintf(r->preedit, sizeof r->preedit, "%s%s", text, roma);
    }
}

static void commit_append(struct ime_result *r, const char *s)
{
    strlcat(r->commit, s, sizeof r->commit);
}

/* choose commits candidate i and removes the text that it replaces. */
static void choose(int i, struct ime_result *r)
{
    commit_append(r, cands[i]);
    memmove(text, text + cand_cover, strlen(text + cand_cover) + 1);
    cands_clear();
}

/* commit_all commits the composition as it is shown. */
static void commit_all(struct ime_result *r)
{
    if (mode == IME_JAPANESE) {
        roma_flush();
        if (ncands) {
            commit_append(r, cands[sel]);
            memmove(text, text + cand_cover, strlen(text + cand_cover) + 1);
        }
        char out[CAND_BYTES * 2];
        if (katakana)
            to_katakana(text, strlen(text), out, sizeof out);
        else
            strlcpy(out, text, sizeof out);
        commit_append(r, out);
    } else {
        commit_append(r, text);
    }
    text[0] = '\0';
    katakana = 0;
    cands_clear();
}

static void delete_last(void)
{
    size_t n = strlen(roma);
    if (n) {
        roma[n - 1] = '\0';
        return;
    }
    n = strlen(text);
    while (n > 0) {
        n--;
        if (((unsigned char)text[n] & 0xc0) != 0x80)
            break;
    }
    text[n] = '\0';
}

int ime_composing(void)
{
    return mode != IME_OFF && (text[0] || roma[0]);
}

/* move_selection moves the selected candidate by d, around the list. */
static void move_selection(int d)
{
    sel = (sel + d + ncands) % ncands;
    cands_changed();
}

static int japanese_key(uint32_t key, int ch, int mods, struct ime_result *r)
{
    if (ncands) {
        if (key == KEY_SPACE || key == KEY_DOWN || key == KEY_RIGHT) {
            move_selection((mods & KEYMAP_MOD_SHIFT) && key == KEY_SPACE ? -1 : 1);
        } else if (key == KEY_UP || key == KEY_LEFT) {
            move_selection(-1);
        } else if (key == KEY_ENTER || key == KEY_KPENTER) {
            choose(sel, r);
        } else if (ch >= '1' && ch <= '9') {
            int i = sel / PAGE * PAGE + ch - '1';
            if (i < ncands)
                choose(i, r);
        } else if (key == KEY_ESC || key == KEY_BACKSPACE) {
            cands_clear();
        } else if (ch >= 32 && !keysym_is_symbol(ch)) {
            /* Typing goes on after the selected candidate. */
            choose(sel, r);
            return japanese_key(key, ch, mods, r);
        }
        result_preedit(r);
        return 1;
    }
    int letter = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch && strchr("-,.[]'", ch));
    if (ch > ' ' && ch < 127 && (letter || ime_composing())) {
        size_t n = strlen(roma);
        if (n + 1 < sizeof roma) {
            roma[n] = (char)(ch >= 'A' && ch <= 'Z' ? ch + 'a' - 'A' : ch);
            roma[n + 1] = '\0';
        }
        roma_convert();
        result_preedit(r);
        return 1;
    }
    if (!ime_composing())
        return 0;
    if (key == KEY_SPACE) {
        roma_flush();
        cands_lookup();
    } else if (key == KEY_ENTER || key == KEY_KPENTER) {
        commit_all(r);
    } else if (key == KEY_BACKSPACE) {
        delete_last();
    } else if (key == KEY_ESC) {
        text[0] = roma[0] = '\0';
        katakana = 0;
    } else if (key == KEY_F7) {
        roma_flush();
        katakana = !katakana;
    }
    result_preedit(r);
    return 1;
}

int ime_key(uint32_t key, int ch, int mods, struct ime_result *r)
{
    r->commit[0] = r->preedit[0] = '\0';
    r->preedit_changed = 0;
    if (mode == IME_OFF)
        return 0;
    /* A shortcut ends the composition as it is shown and goes on to the
     * client. */
    if (mods & (KEYMAP_MOD_CTRL | KEYMAP_MOD_ALT | KEYMAP_MOD_LOGO)) {
        if (ime_composing()) {
            commit_all(r);
            result_preedit(r);
        }
        return 0;
    }
    return japanese_key(key, ch, mods, r);
}

void ime_finish(struct ime_result *r)
{
    r->commit[0] = r->preedit[0] = '\0';
    r->preedit_changed = 0;
    if (ime_composing() || ncands) {
        commit_all(r);
        result_preedit(r);
    }
}

void ime_reset(void)
{
    text[0] = roma[0] = '\0';
    katakana = 0;
    if (ncands)
        cands_clear();
}

int ime_mode(void)
{
    return mode;
}

void ime_set_mode(int m)
{
    ime_reset();
    mode = m;
    if (mode == IME_JAPANESE)
        table_load(&kana_table, "/usr/share/ime/kana.tab");
}

/* ---- the candidate box ---- */

void ime_set_anchor(int x, int y, int h)
{
    if (x == anchor_x && y == anchor_y && h == anchor_h)
        return;
    anchor_x = x;
    anchor_y = y;
    anchor_h = h;
    if (ncands)
        cands_changed();
}

static int text_width(const char *s)
{
    int S = screen_scale;
    return (gfx_text_width_font_scaled(decor_font(), s, -1, S) + S - 1) / S;
}

/* The candidates of the page of the selected one, each as "n text". */
static int page_label(int i, char *out, size_t size)
{
    return snprintf(out, size, "%d %s", i % PAGE + 1, cands[i]);
}

static struct rect box_rect(void)
{
    int first = sel / PAGE * PAGE, w = PAD;
    char label[CAND_BYTES + 8];
    for (int i = first; i < ncands && i < first + PAGE; i++) {
        page_label(i, label, sizeof label);
        w += text_width(label) + 2 * PAD;
    }
    if (ncands > PAGE) {
        snprintf(label, sizeof label, "%d/%d", sel / PAGE + 1, (ncands + PAGE - 1) / PAGE);
        w += text_width(label) + PAD;
    }
    int h = decor_font()->height + 2 * PAD;
    int x = anchor_x, y = anchor_y + anchor_h + 2;
    if (x + w > screen_w)
        x = screen_w - w;
    if (x < 0)
        x = 0;
    if (y + h > screen_h)
        y = anchor_y - h - 2;
    if (y < 0)
        y = 0;
    return (struct rect){ x, y, w, h };
}

void ime_draw(struct rect clip)
{
    if (!ncands || rect_empty(shown_box))
        return;
    struct rect c = rect_intersect(shown_box, clip);
    if (rect_empty(c))
        return;
    const struct theme *t = decor_theme_ptr();
    const struct font *f = decor_font();
    struct painter p;
    painter_init_scaled(&p, &back, t, screen_scale);
    painter_push(&p, c.x, c.y, c.w, c.h);
    struct rect b = shown_box;
    int ox = -c.x, oy = -c.y;
    painter_rounded(&p, b.x + ox, b.y + oy, b.w, b.h, t->color[TC_FIELD], t->color[TC_BORDER]);
    int x = b.x + PAD, first = sel / PAGE * PAGE;
    char label[CAND_BYTES + 8];
    for (int i = first; i < ncands && i < first + PAGE; i++) {
        page_label(i, label, sizeof label);
        int w = text_width(label) + 2 * PAD;
        uint32_t fg = t->color[TC_TEXT];
        if (i == sel) {
            painter_fill(&p, x + ox, b.y + 3 + oy, w, b.h - 6, t->color[TC_SELECTION]);
            fg = t->color[TC_SELECTION_TEXT];
        }
        painter_text_font(&p, f, x + PAD + ox, b.y + PAD + oy, label, fg, 0xffffffffu);
        x += w;
    }
    if (ncands > PAGE) {
        snprintf(label, sizeof label, "%d/%d", sel / PAGE + 1, (ncands + PAGE - 1) / PAGE);
        painter_text_font(&p, f, x + ox, b.y + PAD + oy, label, t->color[TC_TEXT_DISABLED], 0xffffffffu);
    }
    painter_pop(&p);
}

/* ime_describe writes the candidates for the compositor log, the selected
 * one in brackets. */
void ime_describe(char *out, size_t size)
{
    out[0] = '\0';
    for (int i = 0; i < ncands && i < 2 * PAGE; i++) {
        char one[CAND_BYTES + 4];
        snprintf(one, sizeof one, i == sel ? "[%s]" : "%s", cands[i]);
        strlcat(out, one, size);
    }
}
