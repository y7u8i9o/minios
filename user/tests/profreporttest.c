/* Deterministic report tests: no timer distribution or process scheduling
 * assumptions. Also exercise real guest file I/O and the CLI export path. */
#include <minios/profile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>

static int failures;
#define CHECK(c, text) do { if (!(c)) { printf("FAIL: %s\n", text); failures++; } } while (0)

static size_t read_file(const char *path, char *buf, size_t size)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        buf[0] = 0;
        return 0;
    }
    size_t n = fread(buf, 1, size - 1, f);
    buf[n] = 0;
    fclose(f);
    return n;
}

static void test_reports(void)
{
    struct prof_session *s = prof_session_new(NULL, 1000000);
    CHECK(s != NULL, "session allocation");
    if (!s)
        return;
    struct prof_tree *t = &s->view[PROF_VIEW_CPU];
    int a = prof_names_intern(s->names, "alpha");
    int b = prof_names_intern(s->names, "beta");
    int odd = prof_names_intern(s->names, "odd;\"\\\n\t");
    struct prof_stack stack = { .names = {a}, .count = 1 };
    int alpha = prof_tree_add(t, &stack, 10, 3);
    stack.names[1] = b;
    stack.count = 2;
    int beta = prof_tree_add(t, &stack, 30, 7);
    stack.names[1] = a;                 /* recursive alpha */
    int recur = prof_tree_add(t, &stack, 20, 5);
    stack.names[0] = b;                 /* unrelated caller */
    stack.names[1] = a;
    prof_tree_add(t, &stack, 40, 0);
    struct prof_breakdown rows[16];
    size_t n = prof_tree_breakdown(t, alpha, rows, 16);
    CHECK(n == 3, "self plus two direct callees");
    CHECK(rows[0].node == beta && rows[0].total == 30 && rows[1].node == recur &&
          rows[1].total == 20 && rows[2].node == alpha && rows[2].total == 10 && rows[2].extra == 3,
          "breakdown sorted by cost with exclusive self extra");
    uint64_t total = 0;
    for (size_t i = 0; i < n; i++)
        total += rows[i].total;
    CHECK(total == t->nodes[alpha].total && total == 60, "breakdown conserves selected cost");
    CHECK(prof_tree_match_weight(t, 0, "alpha") == 100, "recursive matches counted once");
    CHECK(prof_tree_match_weight(t, alpha, "alpha") == 60, "search stays inside selected caller");
    CHECK(prof_tree_match_weight(t, alpha, "beta") == 30, "search includes matching descendant");
    CHECK(prof_tree_match_weight(t, beta, "alpha") == 0, "ancestors outside zoom do not match");
    CHECK(prof_tree_match_weight(t, 0, "absent") == 0 && prof_tree_match_weight(t, 0, "") == 0,
          "empty and absent search");
    CHECK(prof_tree_breakdown(t, -1, rows, 16) == 0 &&
          prof_tree_breakdown(t, 0, rows, 0) == 0, "invalid and zero-capacity breakdown");
    CHECK(prof_tree_breakdown(t, beta, rows, 16) == 1 && rows[0].total == 30,
          "leaf consists entirely of self");
    prof_tree_sort(t);
    CHECK(prof_tree_match_weight(t, 0, "alpha") == 100, "stable after child reordering");

    stack.count = 1;
    stack.names[0] = odd;
    stack.kernel[0] = 1;
    prof_tree_add(t, &stack, 5, 0);
    stack.count = 0;
    prof_tree_add(t, &stack, 7, 0);       /* no unwind information */
    struct prof_stats stats = { .dropped = 9 };
    const char *json = "/tmp/profreport.json", *folded = "/tmp/profreport.folded";
    CHECK(prof_session_export(s, &stats, json, PROF_EXPORT_JSON, 0) == 0, "JSON export");
    char data[8192];
    CHECK(read_file(json, data, sizeof data) > 0 && strstr(data, "\"version\":1") &&
          strstr(data, "\"dropped\":9") && strstr(data, "odd;\\\"\\\\\\u000a\\u0009") &&
          strstr(data, "\"self\":7") && strstr(data, "\"name\":\"io\""),
          "JSON escaping, metadata, root self, and all views");
    CHECK(prof_session_export(s, NULL, folded, PROF_EXPORT_FOLDED, 0) == 0, "folded export");
    read_file(folded, data, sizeof data);
    CHECK(strstr(data, "alpha;beta 30\n") && strstr(data, "alpha;alpha 20\n") &&
          strstr(data, "odd_\"\\___[k] 5\n") && strstr(data, "[unattributed] 7\n"),
          "folded recursion, sanitized names, kernel marker, and root weight");
    total = 0;
    for (char *line = strtok(data, "\n"); line; line = strtok(NULL, "\n")) {
        char *weight = strrchr(line, ' ');
        CHECK(weight != NULL, "each folded record has a weight");
        if (weight)
            total += strtoull(weight + 1, NULL, 10);
    }
    CHECK(total == 112, "folded weights conserve total including unresolved root");
    stack.count = 1;
    stack.names[0] = a;
    prof_tree_add(&s->view[PROF_VIEW_HEAP], &stack, 4096, 1024);
    CHECK(prof_session_export(s, NULL, folded, PROF_EXPORT_FOLDED, PROF_VIEW_HEAP) == 0,
          "replace export with selected heap view");
    read_file(folded, data, sizeof data);
    CHECK(strcmp(data, "alpha_[k] 4096\n") == 0, "heap exports allocated byte weight");
    CHECK(prof_session_export(s, NULL, json, 123, 0) < 0 && errno == EINVAL,
          "invalid export format refused");
    CHECK(prof_session_export(s, NULL, json, PROF_EXPORT_JSON, PROF_VIEW_COUNT) < 0 && errno == EINVAL,
          "invalid view refused");
    CHECK(prof_session_export(s, NULL, "/missing-directory/export", PROF_EXPORT_JSON, 0) < 0,
          "unwritable path fails");
    char temp[128];
    snprintf(temp, sizeof temp, "%s.tmp.%d", json, getpid());
    int fd = open(temp, O_WRONLY | O_CREAT | O_EXCL, 0600);
    CHECK(fd >= 0, "reserve temporary name");
    if (fd >= 0) close(fd);
    CHECK(prof_session_export(s, NULL, json, PROF_EXPORT_JSON, 0) < 0 && errno == EEXIST,
          "temporary-file collision fails without overwriting it");
    read_file(json, data, sizeof data);
    CHECK(strstr(data, "\"dropped\":9") != NULL, "failed export preserves existing file");
    unlink(temp);
    mkdir("/tmp/profreport-dir", 0700);
    CHECK(prof_session_export(s, NULL, "/tmp/profreport-dir", PROF_EXPORT_JSON, 0) < 0,
          "rename over directory fails");
    snprintf(temp, sizeof temp, "/tmp/profreport-dir.tmp.%d", getpid());
    CHECK(access(temp, F_OK) < 0, "rename failure removes temporary file");
    rmdir("/tmp/profreport-dir");
    prof_session_reset(s);
    CHECK(prof_session_export(s, NULL, json, PROF_EXPORT_JSON, 0) == 0, "empty capture export");
    read_file(json, data, sizeof data);
    CHECK(strstr(data, "\"dropped\":null") && strstr(data, "\"total\":0"), "empty and unknown metadata");
    unlink(json);
    unlink(folded);
    prof_session_free(s);
}

static void test_cli(void)
{
    pid_t child = fork();
    if (child == 0) {
        execl("/bin/prof", "prof", "-a", "-d", "0.1", "-o", "/tmp/profreport-cli.json", (char *)NULL);
        _exit(127);
    }
    int status = 0;
    CHECK(child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "CLI JSON export exits successfully");
    char data[512];
    read_file("/tmp/profreport-cli.json", data, sizeof data);
    CHECK(strstr(data, "\"format\":\"minios-profile\"") != NULL, "CLI wrote JSON capture");
    unlink("/tmp/profreport-cli.json");
}

int main(void)
{
    test_reports();
    test_cli();
    printf("profreporttest: %d failures\n", failures);
    return failures ? 1 : 0;
}
