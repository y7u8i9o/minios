/* installer: install minios from its installation medium onto a disk
 * (docs/design/installer.md). It runs on the console of the installer
 * environment, whose init starts it. When the medium contains an answer file,
 * installer.conf, or one is given with -a FILE, it installs without
 * questions and powers off. Otherwise it asks for the disk, the package
 * group, the language, the keyboard layout, the time zone, the size of
 * swap, the password of root and the first account, shows a summary and
 * installs after the word yes. */
#include "installer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <minios/account.h>

/* Ask a question with a default. An empty answer takes the default. An
 * empty default is not shown. */
static void ask(const char *question, const char *def, char *out, size_t size)
{
    char line[256];
    if (def[0])
        printf("%s [%s]: ", question, def);
    else
        printf("%s: ", question);
    fflush(stdout);
    if (!fgets(line, sizeof line, stdin))
        line[0] = '\0';
    line[strcspn(line, "\n")] = '\0';
    snprintf(out, size, "%s", line[0] ? line : def);
}

/* Ask for a password twice until both agree. An empty one is allowed only
 * when allow_empty is set. */
static void ask_password(const char *who, char *out, size_t size, int allow_empty)
{
    char again[128], prompt[96];
    for (;;) {
        snprintf(prompt, sizeof prompt, "Password for %s: ", who);
        if (account_read_password(prompt, out, size) < 0)
            out[0] = '\0';
        if (!out[0] && allow_empty)
            return;
        if (!out[0]) {
            printf("The password must not be empty. Try again.\n");
            continue;
        }
        snprintf(prompt, sizeof prompt, "Repeat the password for %s: ", who);
        if (account_read_password(prompt, again, sizeof again) == 0 && strcmp(out, again) == 0)
            return;
        printf("The two passwords differ. Try again.\n");
    }
}

static void interactive(struct plan *p, const char *medium_disk)
{
    char names[8][16], buf[64];
    long mib[8];
    int n = inst_list_disks(medium_disk, names, mib, 8);
    printf("Disks:\n");
    for (int i = 0; i < n; i++)
        printf("  %s  %ld MiB\n", names[i], mib[i]);
    printf("The installation erases all data on the chosen disk.\n");
    ask("Install on which disk?", n ? names[0] : "vdb", p->disk, sizeof p->disk);
    printf("\nPackage groups:\n"
           "  desktop-system  the standard system with the graphical desktop\n"
           "  standard        the standard console system\n"
           "  minimal         the minimal bootable system\n");
    ask("Which package group?", "desktop-system", p->group, sizeof p->group);
    ask("Additional packages, separated by spaces (for example apps devel)", "", p->packages,
        sizeof p->packages);
    printf("\n");
    ask("Language", p->lang, p->lang, sizeof p->lang);
    ask("Keyboard layout", p->keymap, p->keymap, sizeof p->keymap);
    ask("Time zone (for example Europe/Berlin)", p->timezone, p->timezone, sizeof p->timezone);
    snprintf(buf, sizeof buf, "%d", p->swap_mb);
    ask("Swap partition size in MiB", buf, buf, sizeof buf);
    p->swap_mb = atoi(buf);
    printf("\n");
    ask_password("root", p->root_password, sizeof p->root_password, 0);
    printf("\nThe first account is a member of the group wheel. Type none to create no account.\n");
    ask("Account name", "user", p->user, sizeof p->user);
    if (strcmp(p->user, "none") == 0)
        p->user[0] = '\0';
    if (p->user[0]) {
        ask("Full name", p->user, p->user_fullname, sizeof p->user_fullname);
        ask_password(p->user, p->user_password, sizeof p->user_password, 0);
    }
}

int main(int argc, char **argv)
{
    struct plan p = { 0 };
    const char *answers = NULL;
    int opt;
    while ((opt = getopt(argc, argv, "a:")) != -1) {
        if (opt != 'a') {
            fprintf(stderr, "usage: installer [-a answers]\n");
            return 2;
        }
        answers = optarg;
    }
    mkdir(INST_DIR, 0755);
    char medium[16] = "";
    if (inst_find_medium(medium, sizeof medium) < 0) {
        inst_log("no installation medium with a repository for this machine was found");
        return 1;
    }
    inst_log("the installation medium is %s", medium);
    struct stat st;
    if (!answers && stat(INST_MEDIUM "/" INST_ANSWERS, &st) == 0)
        answers = INST_MEDIUM "/" INST_ANSWERS;
    if (answers) {
        if (inst_read_answers(answers, &p) < 0) {
            inst_log("%s cannot be read", answers);
            return 1;
        }
        inst_log("installing without questions, as %s asks", answers);
        inst_defaults(&p);
    } else {
        inst_defaults(&p);
        printf("\nInstall minios\n\n");
        interactive(&p, medium);
        printf("\nThe installation erases all data on %s and installs %s on it.\n", p.disk, p.group);
        char yes[16];
        ask("Type yes to start the installation", "no", yes, sizeof yes);
        if (strcmp(yes, "yes") != 0) {
            inst_log("nothing was installed");
            return 1;
        }
        p.poweroff = 1;
    }
    int r = inst_check(&p, medium) < 0 ? -1 : inst_install(&p);
    inst_log(r == 0 ? "done" : "the installation failed, see " INST_LOG);
    /* Without questions the installer powers off in either case, since
     * init would start it again with the same answers. */
    if (r < 0 && answers)
        execl("/usr/bin/initctl", "initctl", "poweroff", (char *)NULL);
    if (r < 0) {
        char line[8];
        printf("Press Enter to start again. ");
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin))
            line[0] = '\0';
    }
    if (r == 0 && p.poweroff) {
        if (!answers) {
            char line[8];
            printf("Remove the installation medium, then press Enter to power off. ");
            fflush(stdout);
            if (!fgets(line, sizeof line, stdin))
                line[0] = '\0';
        }
        execl("/usr/bin/initctl", "initctl", "poweroff", (char *)NULL);
    }
    return r == 0 ? 0 : 1;
}
