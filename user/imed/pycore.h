#pragma once
/* The pinyin core of imed (I3, docs/design/ime.md): the dictionary of
 * user/share/ime/pinyin.dict, the user dictionary, and the candidates of a
 * string of pinyin letters.  It uses only the C library, and the host
 * tests in user/imed/tests run it. */
#include <stddef.h>
#include <stdint.h>

#define PY_MAX_LETTERS 64
#define PY_MAX_WORD_SYLLABLES 8
#define PY_MAX_SENTENCE_SYLLABLES 32

/* A candidate: its text, the number of letters it covers from the start
 * (apostrophes counted), and its syllables. */
struct py_cand {
    char text[96];
    int end;
    uint16_t ids[PY_MAX_SENTENCE_SYLLABLES];
    int nids;
    double score;
};

int py_load(const char *path);                   /* 0, or a negative errno */
int py_user_load(const char *path);              /* the user dictionary; the file may be missing */
void py_user_learn(const char *word, const uint16_t *ids, int nids);
const char *py_syllable(int id);
int py_syllable_count(void);

/* py_candidates fills out with the candidates of letters (a to z and
 * apostrophes): a sentence over all letters when it needs more than one
 * word, then the words at the start, the longer first, each group by
 * falling score.  aux receives the letters divided into syllables with
 * apostrophes.  It returns the number of candidates. */
int py_candidates(const char *letters, struct py_cand *out, int max, char *aux, size_t auxsize);
