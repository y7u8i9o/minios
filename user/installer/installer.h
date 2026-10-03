/* The installer (docs/design/installer.md, P7 of docs/plan/packaging.md):
 * the choices of an installation and the back end that carries it out. */
#pragma once
#include <stddef.h>

#define INST_DIR     "/run/installer"
#define INST_MEDIUM  INST_DIR "/medium"
#define INST_TARGET  INST_DIR "/target"
#define INST_EMPTY   INST_DIR "/empty"
#define INST_LOG     INST_DIR "/installer.log"
#define INST_ANSWERS "installer.conf"

/* The choices, from the questions or from the answer file of the medium.
 * Strings are empty when not given. */
struct plan {
    char disk[16];              /* the target disk, as vdb */
    char group[64];             /* the package group, desktop-system by default */
    char packages[512];         /* further packages, separated by spaces */
    char lang[64];              /* the language of desktop.conf, as en_US.UTF-8 */
    char keymap[32];            /* the keyboard layout of desktop.conf, as us */
    char timezone[64];          /* a zone of /usr/share/zoneinfo, as Europe/Berlin */
    char root_password[128];
    char user[33];              /* the first account, a member of wheel */
    char user_fullname[64];
    char user_password[128];
    char reuse_home[16];        /* an existing data volume mounted on /home, as vdc */
    char cmdline[256];          /* appended to /etc/kernel/cmdline */
    int swap_mb;
    int poweroff;               /* power off when done, the default of the answer file */
};

/* Messages go to the console and to the log, which the installation
 * copies to /var/log/installer.log of the target. */
void inst_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Find the installation medium, a partition of the type repo with an mfs
 * that contains repo/MACHINE/index, and mount it on
 * INST_MEDIUM. Returns 0 with the name of its disk in disk, or -1. */
int inst_find_medium(char *disk, size_t size);
/* Read the answer file at path into p. Returns 0 or -1. */
int inst_read_answers(const char *path, struct plan *p);
/* Fill the defaults of the choices that are empty. */
void inst_defaults(struct plan *p);
/* The disks other than the medium, one name each, and their sizes in
 * MiB. Returns the count. */
int inst_list_disks(const char *medium_disk, char (*names)[16], long *mib, int max);
/* Check the choices. Returns 0, or -1 with a message logged. */
int inst_check(const struct plan *p, const char *medium_disk);
/* Carry out the installation. Returns 0 or -1, after a message. */
int inst_install(const struct plan *p);
