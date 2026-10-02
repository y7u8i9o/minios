#pragma once
/* The Japanese core of imed (I4, docs/design/ime.md): the dictionary of
 * user/share/ime/japanese.dict, the user history, the conversion of a
 * reading into segments and the candidates of a segment.  It uses only
 * the C library, and the host tests in user/imed/tests run it.  Positions
 * are byte offsets into the reading, at character boundaries. */
#include <stddef.h>

#define JP_MAX_CHARS 64
#define JP_SURFACE 160

struct jp_segment {
    int start, end;             /* the part of the reading */
    char surface[JP_SURFACE];
    int lclass, rclass;         /* of its first and last word */
};

struct jp_cand {
    char surface[JP_SURFACE];
    int lclass, rclass;
};

int jp_load(const char *path);                  /* 0, or a negative errno */
int jp_user_load(const char *path);
void jp_user_learn(const char *reading, const char *surface, int lclass, int rclass);

/* jp_convert converts reading[from..] into segments by the lowest cost
 * path.  With first_end > from the first segment ends there.  It returns
 * the number of segments. */
int jp_convert(const char *reading, int from, int first_end, struct jp_segment *out, int max);

/* jp_candidates fills out with the surfaces of reading[start..end): the
 * lowest cost path, the words of the whole reading, the other words of
 * its first word followed by the rest, then hiragana and katakana. */
int jp_candidates(const char *reading, int start, int end, struct jp_cand *out, int max);

void jp_katakana(const char *in, char *out, size_t size);
void jp_halfwidth(const char *in, char *out, size_t size);   /* katakana and letters in half width */
void jp_fullwidth(const char *in, char *out, size_t size);   /* letters in full width */
