/* Extended libc coverage: non-local jumps, locale, UTF-8 wide characters,
 * POSIX regular expressions and the C time additions. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <setjmp.h>
#include <locale.h>
#include <wchar.h>
#include <regex.h>
#include <fnmatch.h>
#include <glob.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <wctype.h>
#include <strings.h>
#include <libgen.h>
#include <err.h>
#include <limits.h>
#include <pwd.h>
#include <grp.h>
#include <sys/sysmacros.h>
#include <dirent.h>
#include <fcntl.h>

static void test_terminal_libc(void);
static void test_stdio_additions(void);
static void test_port_additions(void);

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("libcexttest: FAIL " __VA_ARGS__); printf("\n"); } } while (0)

static jmp_buf jump_target;

static void jump_from_callee(int value)
{
    longjmp(jump_target, value);
}

static void test_setjmp(void)
{
    int value = setjmp(jump_target);
    if (!value)
        jump_from_callee(37);
    CHECK(value == 37, "longjmp returned %d", value);

    value = setjmp(jump_target);
    if (!value)
        jump_from_callee(0);
    CHECK(value == 1, "longjmp zero became %d", value);
}

static void test_locale(void)
{
    CHECK(strcmp(setlocale(LC_ALL, 0), "C") == 0, "locale query");
    CHECK(strcmp(setlocale(LC_CTYPE, "POSIX"), "C") == 0, "POSIX locale alias");
    CHECK(setlocale(LC_ALL, "not-a-locale") == 0, "unknown locale rejected");
    struct lconv *locale = localeconv();
    CHECK(strcmp(locale->decimal_point, ".") == 0 && !*locale->thousands_sep,
          "C numeric locale");
}

static void test_wchar(void)
{
    wchar_t text[16];
    wcscpy(text, L"wide");
    wcscat(text, L" chars");
    CHECK(wcslen(text) == 10 && wcscmp(text, L"wide chars") == 0, "wide strings");
    CHECK(wcsstr(text, L"chars") == text + 5 && wcschr(text, L' ') == text + 4,
          "wide searches");

    const char encoded[] = "A\xc3\xa9\xf0\x9f\x98\x80";
    const char *source = encoded;
    wchar_t decoded[8];
    mbstate_t state = { 0 };
    size_t count = mbsrtowcs(decoded, &source, 8, &state);
    CHECK(count == 3 && source == 0 && decoded[0] == L'A' &&
          decoded[1] == 0xe9 && decoded[2] == 0x1f600,
          "UTF-8 decode count %lu", (unsigned long)count);

    char roundtrip[16];
    const wchar_t *wide_source = decoded;
    memset(&state, 0, sizeof state);
    count = wcsrtombs(roundtrip, &wide_source, sizeof roundtrip, &state);
    CHECK(count == sizeof encoded - 1 && wide_source == 0 &&
          strcmp(roundtrip, encoded) == 0, "UTF-8 round trip");

    wchar_t wc = 0;
    memset(&state, 0, sizeof state);
    CHECK(mbrtowc(&wc, "\xc3", 1, &state) == (size_t)-2 && !mbsinit(&state),
          "partial UTF-8 lead");
    CHECK(mbrtowc(&wc, "\xa9", 1, &state) == 1 && wc == 0xe9 && mbsinit(&state),
          "partial UTF-8 completion");
    errno = 0;
    CHECK(mbrtowc(&wc, "\xc0", 1, &state) == (size_t)-1 && errno == EILSEQ,
          "invalid UTF-8 rejected");

    wchar_t *end;
    CHECK(wcstol(L" -123x", &end, 10) == -123 && *end == L'x', "wcstol");
}

static void test_regex(void)
{
    regex_t regex;
    regmatch_t match[4];
    int error = regcomp(&regex, "^(ab|a)([[:digit:]]{2,3})$", REG_EXTENDED);
    CHECK(error == 0, "compile ERE: %d", error);
    if (!error) {
        CHECK(regex.re_nsub == 2, "ERE subexpression count %lu", (unsigned long)regex.re_nsub);
        error = regexec(&regex, "ab123", 4, match, 0);
        CHECK(error == 0 && match[0].rm_so == 0 && match[0].rm_eo == 5 &&
              match[1].rm_so == 0 && match[1].rm_eo == 2 &&
              match[2].rm_so == 2 && match[2].rm_eo == 5,
              "ERE captures: %d [%ld,%ld] [%ld,%ld] [%ld,%ld]", error,
              match[0].rm_so, match[0].rm_eo, match[1].rm_so, match[1].rm_eo,
              match[2].rm_so, match[2].rm_eo);
        regfree(&regex);
    }

    CHECK(regcomp(&regex, "a|ab", REG_EXTENDED) == 0, "compile alternation");
    CHECK(regexec(&regex, "zab", 1, match, 0) == 0 &&
          match[0].rm_so == 1 && match[0].rm_eo == 3,
          "leftmost-longest alternation [%ld,%ld]", match[0].rm_so, match[0].rm_eo);
    regfree(&regex);

    CHECK(regcomp(&regex, "^[a-z]+$", REG_EXTENDED | REG_ICASE) == 0,
          "compile case-insensitive class");
    CHECK(regexec(&regex, "MiniOS", 0, 0, 0) == 0, "case-insensitive class match");
    regfree(&regex);

    CHECK(regcomp(&regex, "^b$", REG_EXTENDED | REG_NEWLINE) == 0,
          "compile newline anchors");
    CHECK(regexec(&regex, "a\nb\nc", 1, match, 0) == 0 &&
          match[0].rm_so == 2 && match[0].rm_eo == 3,
          "newline anchors [%ld,%ld]", match[0].rm_so, match[0].rm_eo);
    regfree(&regex);

    CHECK(regcomp(&regex, "^\\(ab*\\)x\\1$", 0) == 0, "compile BRE back reference");
    CHECK(regexec(&regex, "abbbxabbb", 2, match, 0) == 0 &&
          match[1].rm_so == 0 && match[1].rm_eo == 4,
          "BRE back reference");
    regfree(&regex);

    CHECK(regcomp(&regex, "b+", REG_EXTENDED) == 0, "compile STARTEND pattern");
    match[0].rm_so = 3;
    match[0].rm_eo = 6;
    CHECK(regexec(&regex, "aaabbbccc", 1, match, REG_STARTEND) == 0 &&
          match[0].rm_so == 3 && match[0].rm_eo == 6, "REG_STARTEND");
    regfree(&regex);

    error = regcomp(&regex, "[z-a]", REG_EXTENDED);
    char message[64];
    CHECK(error == REG_ERANGE && regerror(error, &regex, message, sizeof message) > 1,
          "invalid range error %d: %s", error, message);
}

static void test_time_additions(void)
{
    struct timespec now, resolution;
    CHECK(timespec_get(&now, TIME_UTC) == TIME_UTC && now.tv_sec > 0,
          "timespec_get");
    CHECK(timespec_getres(&resolution, TIME_UTC) == TIME_UTC &&
          resolution.tv_sec == 0 && resolution.tv_nsec > 0,
          "timespec_getres");
    CHECK(timespec_get(&now, 99) == 0, "unknown time base rejected");
    tzset();
    CHECK(strcmp(tzname[0], "UTC") == 0 && timezone == 0 && daylight == 0,
          "UTC timezone state");
}

int main(void)
{
    test_setjmp();
    test_locale();
    test_wchar();
    test_regex();
    test_time_additions();
    test_terminal_libc();
    test_stdio_additions();
    test_port_additions();
    printf("libcexttest: %d failures\n", failures);
    return failures ? 1 : 0;
}

static void test_terminal_libc(void)
{
    CHECK(fnmatch("a*[0-9]?", "abc7x", 0) == 0, "fnmatch range");
    CHECK(fnmatch("[[:alpha:]]", "Z", 0) == 0, "fnmatch class");
    CHECK(fnmatch("*", ".hidden", FNM_PERIOD) == FNM_NOMATCH, "hidden match");
    CHECK(fnmatch("a/*", "a/b/c", FNM_PATHNAME) == FNM_NOMATCH, "pathname match");
    CHECK(fnmatch("a\\*", "a*", 0) == 0, "escaped wildcard");
    CHECK(fnmatch("[!a-c]", "z", 0) == 0, "negated range");
    glob_t g = {0};
    CHECK(glob("/bin/sh", 0, NULL, &g) == 0 && g.gl_pathc == 1 &&
          !strcmp(g.gl_pathv[0], "/bin/sh"), "glob literal");
    globfree(&g);
    CHECK(glob("/bin/*", 0, NULL, &g) == 0 && g.gl_pathc > 10, "glob directory");
    for (size_t i = 1; i < g.gl_pathc; i++)
        CHECK(strcmp(g.gl_pathv[i-1], g.gl_pathv[i]) <= 0, "glob sort");
    globfree(&g);
    CHECK(glob("/no-such-terminal-file*", GLOB_NOCHECK, NULL, &g) == 0 &&
          g.gl_pathc == 1, "glob no match fallback");
    globfree(&g);
    CHECK(wcwidth(L'A') == 1 && wcwidth(0x301) == 0 && wcwidth(0x4e00) == 2 &&
          wcwidth('\n') == -1 && wcswidth(L"A\u0301\u4e00", 3) == 3, "display widths");
    FILE *f = fopen("/getline-test", "w");
    CHECK(f != NULL, "getline fixture");
    if (!f) return;
    for (int i = 0; i < 300; i++) fputc('x', f);
    fputs("\nlast", f); fclose(f);
    f = fopen("/getline-test", "r");
    if (!f) { CHECK(0, "getline reopen"); return; }
    char *line = NULL; size_t capacity = 0;
    CHECK(getline(&line, &capacity, f) == 301 && line[300] == '\n', "getline growth");
    CHECK(getline(&line, &capacity, f) == 4 && !strcmp(line, "last"), "getline final line");
    CHECK(getline(&line, &capacity, f) == -1 && feof(f), "getline EOF");
    free(line); fclose(f); unlink("/getline-test");
}

/* The functions added for the Lua port: push back, stream reopening,
 * temporary files, the shell, collation and the POSIX names. */
static void test_stdio_additions(void)
{
    CHECK(isgraph('a') && !isgraph(' ') && isblank('\t'), "isgraph and isblank");
    CHECK(strcoll("abc", "abd") < 0 && strcoll("b", "a") > 0, "strcoll order");

    FILE *f = tmpfile();
    CHECK(f != NULL, "tmpfile");
    if (!f) return;
    fputs("12 rest\n", f);
    rewind(f);
    int c = fgetc(f);
    CHECK(c == '1' && ungetc('9', f) == '9' && fgetc(f) == '9' && fgetc(f) == '2',
          "ungetc replaces the byte read");
    CHECK(fgetc(f) == ' ' && ftell(f) == 3, "position after push back");
    while (fgetc(f) != EOF) ;
    CHECK(feof(f) && ungetc('x', f) == 'x' && !feof(f) && fgetc(f) == 'x' &&
          fgetc(f) == EOF, "ungetc at end of file");
    fclose(f);

    char name[L_tmpnam], other[L_tmpnam];
    CHECK(tmpnam(name) == name && tmpnam(other) == other && strcmp(name, other) != 0 &&
          !strncmp(name, "/tmp/", 5), "tmpnam distinct names");
    f = fopen(name, "w");
    CHECK(f != NULL, "tmpnam is creatable");
    if (!f) return;
    fputs("first\n", f);
    f = freopen(other, "w+", f);
    CHECK(f != NULL, "freopen");
    if (!f) return;
    fputs("second\n", f);
    rewind(f);
    char line[32];
    CHECK(fgets(line, sizeof line, f) && !strcmp(line, "second\n"), "freopen writes the new file");
    fclose(f);
    f = fopen(name, "r");
    CHECK(f && fgets(line, sizeof line, f) && !strcmp(line, "first\n"), "freopen flushed the old file");
    if (f) fclose(f);
    unlink(name);
    unlink(other);

    char template[] = "/tmp/ext_XXXXXX";
    int fd = mkstemp(template);
    CHECK(fd >= 0 && strncmp(template, "/tmp/ext_", 9) == 0 && strchr(template, 'X') == NULL,
          "mkstemp");
    if (fd >= 0) { close(fd); unlink(template); }

    CHECK(system(NULL) == 1, "system reports a shell");
    int status = system("exit 3");
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 3, "system exit status %d", status);

    f = popen("echo piped; exit 5", "r");
    CHECK(f != NULL, "popen read");
    if (f) {
        CHECK(fgets(line, sizeof line, f) && !strcmp(line, "piped\n"), "popen output");
        status = pclose(f);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 5, "pclose status %d", status);
    }
    f = popen("cat > /tmp/popen-test", "w");
    CHECK(f != NULL, "popen write");
    if (f) {
        fputs("written\n", f);
        CHECK(pclose(f) == 0, "pclose write status");
        f = fopen("/tmp/popen-test", "r");
        CHECK(f && fgets(line, sizeof line, f) && !strcmp(line, "written\n"), "popen wrote");
        if (f) fclose(f);
        unlink("/tmp/popen-test");
    }

    jmp_buf env;
    int value = _setjmp(env);
    if (value == 0)
        _longjmp(env, 4);
    CHECK(value == 4, "_setjmp and _longjmp");
}

static int compare_ints(const void *a, const void *b)
{
    return *(const int *)a - *(const int *)b;
}

/* The functions added for the sed and awk ports. */
static void test_port_additions(void)
{
    int a = 0, b = 0, c = 0;
    char word[16];
    float x = 0;
    CHECK(sscanf("12 -7 0x1f", "%d %d %i", &a, &b, &c) == 3 && a == 12 && b == -7 && c == 31,
          "sscanf integers %d %d %d", a, b, c);
    CHECK(sscanf("pi=3.5 rest", "pi=%f %5s", &x, word) == 2 && x == 3.5f && !strcmp(word, "rest"),
          "sscanf float and string");
    CHECK(sscanf("1970 01 02", "%d %d %d %d", &a, &b, &c, &a) == 3, "sscanf stops at the end");
    CHECK(sscanf("", "%d", &a) == EOF, "sscanf on empty input");
    CHECK(sscanf("abc:def", "%[a-c]:%s", word, word + 8) == 2 && !strcmp(word, "abc") && !strcmp(word + 8, "def"),
          "sscanf scanset");
    int n = 0;
    CHECK(sscanf("xy", "xy%n", &n) == 0 && n == 2, "sscanf %%n");

    char *args[] = { "prog", "-ab", "-c", "value", "file", NULL };
    optind = 1;
    optreset = 1;
    int opt, seen_a = 0, seen_b = 0;
    const char *carg = NULL;
    while ((opt = getopt(5, args, "abc:")) != -1) {
        if (opt == 'a') seen_a = 1;
        if (opt == 'b') seen_b = 1;
        if (opt == 'c') carg = optarg;
    }
    CHECK(seen_a && seen_b && carg && !strcmp(carg, "value") && optind == 4, "getopt parsed options");

    char *text = NULL;
    CHECK(asprintf(&text, "%s-%d", "id", 42) == 5 && text && !strcmp(text, "id-42"), "asprintf");
    free(text);

    char path1[] = "/usr/share/man/", path2[] = "plain", path3[] = "/";
    CHECK(!strcmp(basename(path1), "man"), "basename strips slashes");
    CHECK(!strcmp(dirname(path1), "/usr/share"), "dirname");
    CHECK(!strcmp(dirname(path2), ".") && !strcmp(basename(path3), "/"), "dirname and basename edge cases");

    CHECK(strcasecmp("Hello", "hELLO") == 0 && strncasecmp("abcX", "ABCy", 3) == 0 && strcasecmp("a", "b") < 0,
          "strcasecmp");

    int sorted[] = { 1, 3, 5, 7, 9 };
    int key = 7;
    int *found = bsearch(&key, sorted, 5, sizeof sorted[0], compare_ints);
    key = 4;
    CHECK(found == &sorted[3] && bsearch(&key, sorted, 5, sizeof sorted[0], compare_ints) == NULL, "bsearch");

    srandom(11);
    long r1 = random();
    srandom(11);
    CHECK(r1 == random() && r1 >= 0, "random repeats for a seed");

    CHECK(iswalpha(L'a') && iswalpha(0xe9) && !iswalpha(L'1') && iswdigit(L'7') && iswspace(0xa0),
          "wide character classes");
    CHECK(towupper(L'a') == L'A' && towupper(0xe9) == 0xc9 && towlower(0x410) == 0x430 && towupper(L'1') == L'1',
          "towupper and towlower");
    wchar_t wc = 0;
    char mb[8];
    CHECK(mbtowc(&wc, "\xc3\xa9x", 3) == 2 && wc == 0xe9 && wctomb(mb, 0x20ac) == 3 && mb[0] == '\xe2',
          "mbtowc and wctomb");

    struct stat st;
    CHECK(lstat("/bin/sh", &st) == 0 && S_ISREG(st.st_mode) && fchmod(0, 0644) == 0, "lstat and fchmod");
    CHECK(!strcmp(getprogname(), "libcexttest"), "getprogname is %s", getprogname());

    char *dup = strndup("abcdef", 3);
    CHECK(dup && !strcmp(dup, "abc"), "strndup");
    free(dup);
    char joined[16];
    char *end = stpcpy(stpcpy(joined, "ab"), "cd");
    CHECK(!strcmp(joined, "abcd") && end == joined + 4, "stpcpy");
    char pathbuf[PATH_MAX];
    CHECK(realpath("/bin/../bin/./sh", pathbuf) == pathbuf && !strcmp(pathbuf, "/bin/sh"), "realpath normalizes");
    char *alloc = realpath("/bin//sh", NULL);
    CHECK(alloc && !strcmp(alloc, "/bin/sh"), "realpath allocates");
    free(alloc);
    CHECK(realpath("/bin/nosuchfile", pathbuf) == NULL && errno == ENOENT, "realpath of a missing file");
    CHECK(access("/bin/sh", X_OK) == 0 && access("/bin/nosuchfile", F_OK) < 0, "access");
    char cs[16];
    CHECK(confstr(_CS_PATH, cs, sizeof cs) == 5 && !strcmp(cs, "/bin"), "confstr");

    FILE *tf = fopen("/tmp/utime-test", "w");
    if (tf) fclose(tf);
    struct timespec times[2] = { { 0, UTIME_OMIT }, { 86400 * 365, 0 } };
    CHECK(utimensat(AT_FDCWD, "/tmp/utime-test", times, 0) == 0 && stat("/tmp/utime-test", &st) == 0
          && st.st_mtim.tv_sec == 86400 * 365 && st.st_mtim.tv_nsec == 0, "utimensat sets a time");
    times[1].tv_nsec = UTIME_NOW;
    CHECK(utimensat(AT_FDCWD, "/tmp/utime-test", times, 0) == 0 && stat("/tmp/utime-test", &st) == 0
          && st.st_mtime > 1600000000, "utimensat with UTIME_NOW");
    CHECK(utimensat(AT_FDCWD, "/tmp/nosuchfile", NULL, 0) < 0 && errno == ENOENT, "utimensat of a missing file");
    unlink("/tmp/utime-test");

    int dir = open("/bin", O_RDONLY | O_DIRECTORY);
    CHECK(dir >= 0, "open /bin as a directory");
    int fd = openat(dir, "sh", O_RDONLY);
    CHECK(fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode), "openat relative to a directory");
    if (fd >= 0) close(fd);
    CHECK(fstatat(dir, "sh", &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(st.st_mode), "fstatat relative");
    CHECK(fstatat(dir, "/bin/sh", &st, 0) == 0, "fstatat with an absolute path");
    CHECK(fstatat(AT_FDCWD, "/bin/sh", &st, 0) == 0, "fstatat from the working directory");
    CHECK(openat(dir, "nosuchfile", O_RDONLY) < 0 && errno == ENOENT, "openat of a missing name");
    fd = open("/bin/sh", O_RDONLY);
    CHECK(fd >= 0 && openat(fd, "x", O_RDONLY) < 0 && errno == ENOTDIR, "openat with a file descriptor");
    if (fd >= 0) close(fd);
    DIR *dp = fdopendir(dir);
    int saw_sh = 0;
    struct dirent *de;
    while (dp && (de = readdir(dp)) != NULL)
        if (!strcmp(de->d_name, "sh")) saw_sh = 1;
    CHECK(saw_sh, "fdopendir over an openat directory");
    if (dp) closedir(dp);

    struct passwd *pw = getpwuid(0);
    struct group *gr = getgrgid(0);
    CHECK(pw && !strcmp(pw->pw_name, "user") && !strcmp(pw->pw_dir, "/home") && getpwuid(7) == NULL, "getpwuid");
    CHECK(gr && !strcmp(gr->gr_name, "user") && getgrnam("nobody") == NULL, "getgrgid and getgrnam");
    CHECK(getuid() == 0 && geteuid() == 0 && getgid() == 0, "single user ids");
    CHECK(major(makedev(5, 9)) == 5 && minor(makedev(5, 9)) == 9, "device numbers");
    CHECK(symlink("/bin/sh", "/tmp/link") < 0 && errno == EPERM, "symlink is refused");
    char lbuf[16];
    CHECK(readlink("/bin/sh", lbuf, sizeof lbuf) < 0 && errno == EINVAL, "readlink is refused");
    CHECK(mkfifo("/tmp/fifo", 0644) < 0 && errno == EPERM, "mkfifo is refused");

    pid_t child = fork();
    if (child == 0) {
        execlp("echo", "echo", "-n", "", NULL);
        _exit(127);
    }
    int wstatus = -1;
    CHECK(child > 0 && waitpid(child, &wstatus, 0) == child && WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0, "execlp");
    CHECK(strcasecmp(getprogname(), "LIBCEXTTEST") == 0, "program name compares");
}
