/* Host tests of the Japanese core of imed (I4, docs/design/ime.md), run by
 * make check with the dictionary of user/share/ime. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../jpcore.h"

static int failures;

static void convert(const char *reading, char *out, size_t size, int show)
{
    struct jp_segment s[32];
    int n = jp_convert(reading, 0, 0, s, 32);
    out[0] = '\0';
    if (show)
        printf("%-34s", reading);
    for (int i = 0; i < n; i++) {
        strlcat(out, s[i].surface, size);
        if (show)
            printf(" %s", s[i].surface);
    }
    if (show)
        printf("\n");
}

static void expect(const char *reading, const char *want)
{
    char got[512];
    convert(reading, got, sizeof got, 1);
    if (strcmp(got, want) != 0) {
        printf("FAIL %s: %s, expected %s\n", reading, got, want);
        failures++;
    }
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "user/share/ime/japanese.dict";
    if (jp_load(path) < 0) {
        printf("FAIL cannot load %s\n", path);
        return 1;
    }
    char user[] = "/tmp/test_japanese_user.XXXXXX";
    int fd = mkstemp(user);
    if (fd >= 0)
        close(fd);
    jp_user_load(user);
    expect("にほんご", "日本語");
    expect("わたしはがくせいです", "私は学生です");
    expect("きょうはいいてんきですね", "今日はいい天気ですね");
    expect("とうきょうにいきます", "東京に行きます");
    const char *more[] = { "にほんごをべんきょうしています", "かのじょはとしょかんでほんをよんだ", "こんにちは",
                           "ありがとうございます", "かんじへんかん", NULL };
    char got[512];
    for (int i = 0; more[i]; i++)
        convert(more[i], got, sizeof got, 1);
    /* The candidates of a segment, and the learning of one of them. */
    struct jp_cand c[64];
    const char *r = "かんじ";
    int n = jp_candidates(r, 0, (int)strlen(r), c, 64);
    printf("%s:", r);
    for (int i = 0; i < n && i < 10; i++)
        printf(" %s", c[i].surface);
    printf("\n");
    if (n > 3) {
        char want[160];
        strlcpy(want, c[3].surface, sizeof want);
        jp_user_learn(r, c[3].surface, c[3].lclass, c[3].rclass);
        jp_user_learn(r, c[3].surface, c[3].lclass, c[3].rclass);
        jp_user_load(user);
        expect(r, want);
    }
    char k[64], h[64], f[64];
    jp_katakana("にほんご", k, sizeof k);
    jp_halfwidth("にほんご", h, sizeof h);
    jp_fullwidth("abc", f, sizeof f);
    if (strcmp(k, "ニホンゴ") || strcmp(h, "ﾆﾎﾝｺﾞ") || strcmp(f, "ａｂｃ")) {
        printf("FAIL forms %s %s %s\n", k, h, f);
        failures++;
    }
    unlink(user);
    if (failures) {
        printf("japanese: %d failures\n", failures);
        return 1;
    }
    printf("japanese: host checks passed\n");
    return 0;
}
