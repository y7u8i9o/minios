/* Host tests of the pinyin core of imed (I3, docs/design/ime.md), run by
 * make check with the dictionary of user/share/ime. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../pycore.h"

static int failures;

static void show(const char *letters, int max)
{
    struct py_cand c[64];
    char aux[128];
    int n = py_candidates(letters, c, 64, aux, sizeof aux);
    printf("%-22s %-26s", letters, aux);
    for (int i = 0; i < n && i < max; i++)
        printf(" %s/%d", c[i].text, c[i].end);
    printf("\n");
}

/* expect checks that want is among the first rank candidates of letters. */
static void expect(const char *letters, const char *want, int rank)
{
    struct py_cand c[64];
    char aux[128];
    int n = py_candidates(letters, c, 64, aux, sizeof aux);
    for (int i = 0; i < n && i < rank; i++)
        if (strcmp(c[i].text, want) == 0)
            return;
    printf("FAIL %s: %s is not among the first %d candidates (first %s)\n", letters, want, rank, n ? c[0].text : "none");
    failures++;
}

static void expect_aux(const char *letters, const char *want)
{
    struct py_cand c[64];
    char aux[128];
    py_candidates(letters, c, 64, aux, sizeof aux);
    if (strcmp(aux, want) != 0) {
        printf("FAIL %s: syllables %s, expected %s\n", letters, aux, want);
        failures++;
    }
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "user/share/ime/pinyin.dict";
    if (py_load(path) < 0) {
        printf("FAIL cannot load %s\n", path);
        return 1;
    }
    char user[] = "/tmp/test_pinyin_user.XXXXXX";
    int fd = mkstemp(user);
    if (fd >= 0)
        close(fd);
    py_user_load(user);
    const char *samples[] = { "zhongguo", "nihao", "zg", "xian", "xi'an", "jintiantianqihenhao", "woaibeijing",
                              "shuru", "shurufa", "zhon", "nh", "women", "fangan", "pinyin", NULL };
    for (int i = 0; samples[i]; i++)
        show(samples[i], 6);
    expect("zhongguo", "中国", 1);
    expect("nihao", "你好", 1);
    expect("zg", "中国", 3);
    expect("xi'an", "西安", 1);
    expect("jintiantianqihenhao", "今天天气很好", 1);
    expect("shurufa", "输入法", 1);
    expect("women", "我们", 1);
    expect_aux("zhongguo", "zhong'guo");
    expect_aux("xian", "xian");
    expect_aux("xi'an", "xi'an");
    /* Learning: a chosen later candidate comes first. */
    struct py_cand c[64];
    int n = py_candidates("shi", c, 64, NULL, 0);
    if (n > 5) {
        char want[96];
        strlcpy(want, c[5].text, sizeof want);
        for (int i = 0; i < 3; i++)
            py_user_learn(c[5].text, c[5].ids, c[5].nids);
        expect("shi", want, 1);
        py_user_load(user);
        expect("shi", want, 1);
    }
    unlink(user);
    if (failures) {
        printf("pinyin: %d failures\n", failures);
        return 1;
    }
    printf("pinyin: host checks passed\n");
    return 0;
}
