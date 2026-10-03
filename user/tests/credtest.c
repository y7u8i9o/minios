/* U0 test: process credentials and the account databases. Started as root
 * by the user_cred case, it checks the identity calls, drops to uid 1000 in
 * children and checks that the drop sticks across fork and exec, parses
 * account files and checks the password helpers. With "--check UID GID" it
 * is the exec'd child that verifies its inherited ids. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <pwd.h>
#include <grp.h>
#include <shadow.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <minios/account.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

/* Run fn in a child and return its exit status, or -1 when it did not
 * exit normally. */
static int in_child(int (*fn)(void))
{
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0)
        _exit(fn());
    int status = 0;
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status))
        return -1;
    return WEXITSTATUS(status);
}

static int ids_are(uid_t r, uid_t e, uid_t s, gid_t rg, gid_t eg, gid_t sg)
{
    uid_t a, b, c;
    gid_t x, y, z;
    if (getresuid(&a, &b, &c) < 0 || getresgid(&x, &y, &z) < 0)
        return 0;
    return a == r && b == e && c == s && x == rg && y == eg && z == sg;
}

static int check_mode(char **argv)
{
    uid_t uid = (uid_t)strtoul(argv[2], NULL, 10);
    gid_t gid = (gid_t)strtoul(argv[3], NULL, 10);
    if (!ids_are(uid, uid, uid, gid, gid, gid)) {
        printf("credtest --check: ids %u/%u/%u, want %u\n", getuid(), geteuid(), getgid(), uid);
        return 1;
    }
    gid_t groups[NGROUPS_MAX];
    int n = getgroups(NGROUPS_MAX, groups);
    if (n != 2 || groups[0] != 1000 || groups[1] != 50) {
        printf("credtest --check: %d groups after exec\n", n);
        return 2;
    }
    return 0;
}

static void test_root(void)
{
    CHECK(ids_are(0, 0, 0, 0, 0, 0), "boot test does not start as root");
    CHECK(getgroups(0, NULL) == 0, "root starts with supplementary groups");
    CHECK(umask(077) == 022, "initial umask is not 022");
    CHECK(umask(022) == 077, "umask did not return the previous mask");
    CHECK(umask(01777) == 022 && umask(022) == 0777, "umask retains bits beyond 0777");
}

/* The row of pid in /dev/proc reports uid in its UID column. */
static int proc_uid_of(pid_t pid)
{
    FILE *f = fopen("/dev/proc", "r");
    if (!f)
        return -1;
    char line[256];
    int uid = -1;
    while (fgets(line, sizeof line, f)) {
        int p, ppid, pgid;
        char state[16];
        unsigned long ticks, rss;
        unsigned u;
        if (sscanf(line, "%d %d %d %15s %lu %lu %u", &p, &ppid, &pgid, state, &ticks, &rss, &u) == 7 && p == pid) {
            uid = (int)u;
            break;
        }
    }
    fclose(f);
    return uid;
}

static int grandchild_ids(void)
{
    return ids_are(1000, 1000, 1000, 1000, 1000, 1000) ? 0 : 1;
}

/* A child that drops all ids to 1000 the way login does. */
static int drop_child(void)
{
    gid_t groups[2] = { 1000, 50 };
    if (setgroups(2, groups) < 0 || setresgid(1000, 1000, 1000) < 0 || setresuid(1000, 1000, 1000) < 0)
        return 10;
    if (!ids_are(1000, 1000, 1000, 1000, 1000, 1000))
        return 11;
    if (setuid(0) == 0 || errno != EPERM)
        return 12;
    if (setresuid((uid_t)-1, 0, (uid_t)-1) == 0 || errno != EPERM)
        return 13;
    if (setgroups(0, NULL) == 0 || errno != EPERM)
        return 14;
    if (setgid(0) == 0 || errno != EPERM)
        return 15;
    gid_t got[NGROUPS_MAX];
    if (getgroups(NGROUPS_MAX, got) != 2 || got[1] != 50)
        return 16;
    if (getgroups(1, got) != -1 || errno != EINVAL)
        return 17;
    if (proc_uid_of(getpid()) != 1000)
        return 18;
    if (in_child(grandchild_ids) != 0)
        return 19;
    /* An unprivileged process may still swap among its own ids. */
    if (setuid(1000) < 0)
        return 20;
    fflush(stdout);
    execl("/bin/credtest", "credtest", "--check", "1000", "1000", (char *)NULL);
    return 21;
}

/* A child that retains root in the saved uid, as a setuid program does, and
 * moves between the effective ids. */
static int saved_child(void)
{
    if (setresuid(1000, 1000, 0) < 0)
        return 30;
    if (seteuid(0) < 0 || geteuid() != 0)
        return 31;
    if (seteuid(1000) < 0 || geteuid() != 1000)
        return 32;
    if (setresuid(2000, (uid_t)-1, (uid_t)-1) == 0 || errno != EPERM)
        return 33;
    /* setreuid with a real id makes the saved id follow the effective one. */
    if (setreuid(1000, 1000) < 0 || !ids_are(1000, 1000, 1000, 0, 0, 0))
        return 34;
    if (seteuid(0) == 0)
        return 35;
    return 0;
}

static void test_drop(void)
{
    int status = in_child(drop_child);
    CHECK(status == 0, "dropping to uid 1000: step %d", status);
    status = in_child(saved_child);
    CHECK(status == 0, "saved uid: step %d", status);
    CHECK(ids_are(0, 0, 0, 0, 0, 0), "the parent lost root");
}

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static char *read_file(const char *path)
{
    static char buf[1024];
    FILE *f = fopen(path, "r");
    if (!f)
        return NULL;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static void test_parsers(void)
{
    write_file("/tmp/passwd", "# comment\n"
                              "root:x:0:0:Superuser:/root:/bin/sh\n"
                              "broken:x:abc:0::/:/bin/sh\n"
                              "short:x:5\n"
                              "anna:x:1001:1001:Anna Example:/home/anna:/bin/sh\n");
    FILE *f = fopen("/tmp/passwd", "r");
    CHECK(f != NULL, "cannot open /tmp/passwd");
    if (f) {
        struct passwd *pw = fgetpwent(f);
        CHECK(pw && strcmp(pw->pw_name, "root") == 0 && pw->pw_uid == 0 && strcmp(pw->pw_dir, "/root") == 0,
              "first passwd entry");
        pw = fgetpwent(f);
        CHECK(pw && strcmp(pw->pw_name, "anna") == 0 && pw->pw_uid == 1001 && pw->pw_gid == 1001 &&
                  strcmp(pw->pw_gecos, "Anna Example") == 0 && strcmp(pw->pw_shell, "/bin/sh") == 0,
              "malformed passwd lines were not skipped");
        CHECK(fgetpwent(f) == NULL, "passwd entries after the end");
        fclose(f);
    }
    write_file("/tmp/group", "wheel:x:10:anna,root\nanna:x:1001:\nbad:x::\n");
    f = fopen("/tmp/group", "r");
    if (f) {
        struct group *gr = fgetgrent(f);
        CHECK(gr && gr->gr_gid == 10 && gr->gr_mem[0] && strcmp(gr->gr_mem[0], "anna") == 0 && gr->gr_mem[1] &&
                  strcmp(gr->gr_mem[1], "root") == 0 && !gr->gr_mem[2],
              "group members");
        gr = fgetgrent(f);
        CHECK(gr && gr->gr_gid == 1001 && !gr->gr_mem[0], "group without members");
        CHECK(fgetgrent(f) == NULL, "malformed group line accepted");
        fclose(f);
    }
    write_file("/tmp/shadow", "root::19000:0:99999:7:::\nanna:$5$x$y\n");
    f = fopen("/tmp/shadow", "r");
    if (f) {
        struct spwd *sp = fgetspent(f);
        CHECK(sp && strcmp(sp->sp_namp, "root") == 0 && sp->sp_pwdp[0] == '\0' && sp->sp_lstchg == 19000 &&
                  sp->sp_inact == -1,
              "shadow fields");
        sp = fgetspent(f);
        CHECK(sp && strcmp(sp->sp_pwdp, "$5$x$y") == 0 && sp->sp_max == -1, "short shadow line");
        fclose(f);
    }

    struct passwd *pw = getpwnam("user");
    CHECK(pw && pw->pw_uid == 1000 && pw->pw_gid == 1000, "getpwnam(user) on /etc/passwd");
    pw = getpwuid(0);
    CHECK(pw && strcmp(pw->pw_name, "root") == 0, "getpwuid(0)");
    CHECK(getpwnam("nobody-here") == NULL && errno == ENOENT, "unknown account found");
    struct group *gr = getgrgid(1000);
    CHECK(gr && strcmp(gr->gr_name, "user") == 0, "getgrgid(1000)");
    int count = 0;
    setpwent();
    while (getpwent())
        count++;
    endpwent();
    CHECK(count == 2, "getpwent walked %d accounts", count);
    gid_t groups[4];
    int n = 4;
    /* user belongs to its own group and to wheel, which may use doas
     * and sudo (U5). */
    CHECK(getgrouplist("user", 1000, groups, &n) == 2 && groups[0] == 1000 && groups[1] == 10,
          "getgrouplist(user)");
    n = 0;
    CHECK(getgrouplist("user", 5, groups, &n) == -1 && n == 2, "getgrouplist with no room");
    struct spwd *sp = getspnam("user");
    CHECK(sp && sp->sp_pwdp[0] == '\0', "getspnam(user)");
}

static void test_passwords(void)
{
    CHECK(account_check("", ""), "empty hash refused the empty password");
    CHECK(!account_check("x", ""), "empty hash accepted a password");
    CHECK(!account_check("", "!"), "locked hash accepted");
    char hash[128], again[128];
    CHECK(account_hash("secret", hash, sizeof hash) == 0 && strncmp(hash, "$5$", 3) == 0, "account_hash");
    CHECK(account_check("secret", hash), "hash does not verify");
    CHECK(!account_check("Secret", hash), "wrong password verified");
    CHECK(account_hash("secret", again, sizeof again) == 0 && strcmp(hash, again) != 0, "salts repeat");
    const char *c = crypt("Hello world!", "$5$saltstring");
    CHECK(c && strcmp(c, "$5$saltstring$5B8vYYiY.CVt1RlTTf8KbXBH3hsxY/GNooZaBBGWEc5") == 0, "crypt vector");
    CHECK(account_name_valid("anna") && account_name_valid("a_b-9") && !account_name_valid("Anna") &&
              !account_name_valid("9a") && !account_name_valid("") && !account_name_valid("a:b"),
          "account_name_valid");
}

static void test_replace(void)
{
    write_file("/tmp/accounts", "root:x:0:0::/root:/bin/sh\nanna:x:1001:1001::/home/anna:/bin/sh\n");
    unlink("/tmp/accounts-link");
    CHECK(symlink("/tmp/accounts", "/tmp/accounts-link") == 0, "symlink");
    CHECK(account_replace("/tmp/accounts-link", "anna", "anna:x:1001:1001:Anna:/home/anna:/bin/sh") == 0,
          "replace through a link: %s", strerror(errno));
    CHECK(account_replace("/tmp/accounts-link", "bert", "bert:x:1002:1002::/home/bert:/bin/sh") == 0, "append");
    CHECK(account_replace("/tmp/accounts-link", "root", NULL) == 0, "remove");
    char *text = read_file("/tmp/accounts");
    CHECK(text && strcmp(text, "anna:x:1001:1001:Anna:/home/anna:/bin/sh\nbert:x:1002:1002::/home/bert:/bin/sh\n") == 0,
          "edited file: %s", text ? text : "(none)");
    struct stat st;
    CHECK(lstat("/tmp/accounts-link", &st) == 0 && S_ISLNK(st.st_mode), "the link was replaced by a file");
    CHECK(access("/tmp/accounts.new", F_OK) < 0, "temporary file left behind");
    unlink("/tmp/accounts-link");
    unlink("/tmp/accounts");
}

int main(int argc, char **argv)
{
    if (argc == 4 && strcmp(argv[1], "--check") == 0)
        return check_mode(argv);
    test_root();
    test_drop();
    test_parsers();
    test_passwords();
    test_replace();
    printf("credtest: %d failures\n", failures);
    return failures ? 1 : 0;
}
