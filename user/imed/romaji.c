/* Romaji to hiragana for the Japanese engine of imed (I4,
 * docs/design/ime.md), with the table of the L6 engine.  A sequence that
 * begins a longer one waits, a doubled consonant is a small tsu, an n
 * before a consonant is ん, and a letter that begins no sequence remains as
 * it is. */
#include <string.h>
#include "romaji.h"

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

static void append(char *kana, size_t size, const char *s)
{
    if (strlen(kana) + strlen(s) < size)
        strcat(kana, s);
}

static void convert(char *kana, size_t size, char *roma)
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
            append(kana, size, romaji[exact][1]);
            roma[0] = '\0';
            return;
        }
        if (longer)
            return;
        if (n >= 2 && roma[0] == roma[1] && roma[0] != 'n' && !strchr("aiueo", roma[0])) {
            append(kana, size, "っ");
        } else if (roma[0] == 'n') {
            append(kana, size, "ん");
        } else {
            char one[2] = { roma[0], '\0' };
            append(kana, size, one);
        }
        memmove(roma, roma + 1, n);
    }
}

void romaji_feed(char *kana, size_t size, char *roma, size_t rsize, char c)
{
    size_t n = strlen(roma);
    if (n + 1 < rsize) {
        roma[n] = c >= 'A' && c <= 'Z' ? (char)(c + 'a' - 'A') : c;
        roma[n + 1] = '\0';
    }
    convert(kana, size, roma);
}

void romaji_flush(char *kana, size_t size, char *roma)
{
    convert(kana, size, roma);
    append(kana, size, strcmp(roma, "n") == 0 ? "ん" : roma);
    roma[0] = '\0';
}
