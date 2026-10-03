/* The account databases /etc/passwd, /etc/group and /etc/shadow
 * (docs/design/users.md). Each lookup reads the file from the start, and
 * the results live in static storage that the next call overwrites. */
#include <pwd.h>
#include <grp.h>
#include <shadow.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <minios/account.h>

#define LINE_MAX_ACCOUNT 1024
#define MEMBERS_MAX      64

/* Split line at colons into at most max fields, in place, and drop the
 * newline. Returns the number of fields. */
static int split_fields(char *line, char **fields, int max)
{
    line[strcspn(line, "\n")] = '\0';
    int n = 0;
    char *p = line;
    while (n < max) {
        fields[n++] = p;
        char *colon = strchr(p, ':');
        if (!colon)
            break;
        *colon = '\0';
        p = colon + 1;
    }
    return n;
}

/* A decimal id field. Empty fields and trailing garbage are rejected. */
static int parse_id(const char *s, unsigned *out)
{
    if (!*s)
        return -1;
    char *end;
    unsigned long v = strtoul(s, &end, 10);
    if (*end || v > 0xfffffffeUL)
        return -1;
    *out = (unsigned)v;
    return 0;
}

static long parse_long(const char *s)
{
    return *s ? strtol(s, NULL, 10) : -1;
}

/* passwd */

static char pw_line[LINE_MAX_ACCOUNT];
static struct passwd pw_entry;
static FILE *pw_stream;

struct passwd *fgetpwent(FILE *stream)
{
    while (fgets(pw_line, sizeof pw_line, stream)) {
        char *f[7];
        unsigned uid, gid;
        if (pw_line[0] == '#' || split_fields(pw_line, f, 7) != 7 || !f[0][0] || parse_id(f[2], &uid) < 0 ||
            parse_id(f[3], &gid) < 0)
            continue;
        pw_entry = (struct passwd){ f[0], f[1], uid, gid, f[4], f[5], f[6] };
        return &pw_entry;
    }
    return NULL;
}

void setpwent(void)
{
    if (pw_stream)
        rewind(pw_stream);
}

void endpwent(void)
{
    if (pw_stream)
        fclose(pw_stream);
    pw_stream = NULL;
}

struct passwd *getpwent(void)
{
    if (!pw_stream && !(pw_stream = fopen(ACCOUNT_PASSWD, "r")))
        return NULL;
    return fgetpwent(pw_stream);
}

/* Search /etc/passwd for an entry with the given name, or with uid when
 * name is NULL. */
static struct passwd *find_passwd(const char *name, uid_t uid)
{
    FILE *f = fopen(ACCOUNT_PASSWD, "r");
    if (!f)
        return NULL;
    struct passwd *pw;
    while ((pw = fgetpwent(f)))
        if (name ? strcmp(pw->pw_name, name) == 0 : pw->pw_uid == uid)
            break;
    fclose(f);
    if (!pw)
        errno = ENOENT;
    return pw;
}

struct passwd *getpwnam(const char *name)
{
    return find_passwd(name, 0);
}

struct passwd *getpwuid(uid_t uid)
{
    return find_passwd(NULL, uid);
}

/* group */

static char gr_line[LINE_MAX_ACCOUNT];
static char *gr_members[MEMBERS_MAX + 1];
static struct group gr_entry;
static FILE *gr_stream;

struct group *fgetgrent(FILE *stream)
{
    while (fgets(gr_line, sizeof gr_line, stream)) {
        char *f[4];
        unsigned gid;
        if (gr_line[0] == '#' || split_fields(gr_line, f, 4) != 4 || !f[0][0] || parse_id(f[2], &gid) < 0)
            continue;
        int n = 0;
        for (char *m = f[3]; *m && n < MEMBERS_MAX;) {
            gr_members[n++] = m;
            char *comma = strchr(m, ',');
            if (!comma)
                break;
            *comma = '\0';
            m = comma + 1;
        }
        gr_members[n] = NULL;
        gr_entry = (struct group){ f[0], f[1], gid, gr_members };
        return &gr_entry;
    }
    return NULL;
}

void setgrent(void)
{
    if (gr_stream)
        rewind(gr_stream);
}

void endgrent(void)
{
    if (gr_stream)
        fclose(gr_stream);
    gr_stream = NULL;
}

struct group *getgrent(void)
{
    if (!gr_stream && !(gr_stream = fopen(ACCOUNT_GROUP, "r")))
        return NULL;
    return fgetgrent(gr_stream);
}

static struct group *find_group(const char *name, gid_t gid)
{
    FILE *f = fopen(ACCOUNT_GROUP, "r");
    if (!f)
        return NULL;
    struct group *gr;
    while ((gr = fgetgrent(f)))
        if (name ? strcmp(gr->gr_name, name) == 0 : gr->gr_gid == gid)
            break;
    fclose(f);
    if (!gr)
        errno = ENOENT;
    return gr;
}

struct group *getgrnam(const char *name)
{
    return find_group(name, 0);
}

struct group *getgrgid(gid_t gid)
{
    return find_group(NULL, gid);
}

int getgrouplist(const char *user, gid_t group, gid_t *groups, int *ngroups)
{
    int max = *ngroups, n = 0;
    if (n < max)
        groups[n] = group;
    n++;
    FILE *f = fopen(ACCOUNT_GROUP, "r");
    if (f) {
        struct group *gr;
        while ((gr = fgetgrent(f))) {
            if (gr->gr_gid == group)
                continue;
            for (char **m = gr->gr_mem; *m; m++)
                if (strcmp(*m, user) == 0) {
                    if (n < max)
                        groups[n] = gr->gr_gid;
                    n++;
                    break;
                }
        }
        fclose(f);
    }
    *ngroups = n;
    return n <= max ? n : -1;
}

int initgroups(const char *user, gid_t group)
{
    gid_t groups[NGROUPS_MAX];
    int n = NGROUPS_MAX;
    if (getgrouplist(user, group, groups, &n) < 0)
        n = NGROUPS_MAX;
    return setgroups((size_t)n, groups);
}

/* shadow */

static char sp_line[LINE_MAX_ACCOUNT];
static struct spwd sp_entry;

struct spwd *fgetspent(FILE *stream)
{
    while (fgets(sp_line, sizeof sp_line, stream)) {
        char *f[9];
        int n = split_fields(sp_line, f, 9);
        if (sp_line[0] == '#' || n < 2 || !f[0][0])
            continue;
        for (int i = n; i < 9; i++)
            f[i] = "";
        sp_entry = (struct spwd){ f[0], f[1], parse_long(f[2]), parse_long(f[3]), parse_long(f[4]),
                                  parse_long(f[5]), parse_long(f[6]), parse_long(f[7]),
                                  (unsigned long)parse_long(f[8]) };
        return &sp_entry;
    }
    return NULL;
}

struct spwd *getspnam(const char *name)
{
    FILE *f = fopen(ACCOUNT_SHADOW, "r");
    if (!f)
        return NULL;
    struct spwd *sp;
    while ((sp = fgetspent(f)))
        if (strcmp(sp->sp_namp, name) == 0)
            break;
    fclose(f);
    if (!sp)
        errno = ENOENT;
    return sp;
}

/* getlogin names the user of the session: LOGNAME as set by login and
 * the greeter, else the account of the real uid. */
char *getlogin(void)
{
    static char name[32];
    const char *env = getenv("LOGNAME");
    if (env && *env) {
        snprintf(name, sizeof name, "%s", env);
        return name;
    }
    struct passwd *pw = getpwuid(getuid());
    if (!pw)
        return NULL;
    snprintf(name, sizeof name, "%s", pw->pw_name);
    return name;
}
