#pragma once
/* The launcher tables: lines of the form "title=command", blank lines and
 * "#" comments ignored.  The command is a program followed by arguments
 * separated by spaces.  A command that starts with "@" is an action of
 * the panel, such as "@logout".  The tables are PKG_LAUNCHER for
 * installed packages, ~/.local/share/launcher for packages of the user,
 * and /etc/launcher or the user's ~/.config/launcher for the system
 * entries (docs/design/packages.md). */

#define LAUNCHER_TITLE 96
#define LAUNCHER_COMMAND 128
#define LAUNCHER_SYSTEM "/etc/launcher"

struct launcher_entry {
    char title[LAUNCHER_TITLE];
    char command[LAUNCHER_COMMAND];
};

/* launcher_read_table appends the entries of the table at path to
 * entries, an array of max elements with count elements in use.  With
 * translate set, the titles pass through the "launcher" text domain.  The
 * result is the new number of elements in use.  A missing table adds
 * nothing. */
int launcher_read_table(const char *path, int translate, struct launcher_entry *entries, int count, int max);
/* launcher_read_apps reads the tables of packages, PKG_LAUNCHER and then
 * ~/.local/share/launcher, with translated titles in the order of the
 * titles.  The result is the number of entries. */
int launcher_read_apps(struct launcher_entry *entries, int max);
/* launcher_system_path copies the path of the system table to buf: the
 * user's ~/.config/launcher when it exists, else LAUNCHER_SYSTEM. */
const char *launcher_system_path(char *buf, int size);
/* launcher_program copies the program of command, its first word, to buf. */
const char *launcher_program(const char *command, char *buf, int size);
/* launcher_icon_name copies the name of the icon of a command or a program
 * to buf: app-NAME when /usr/share/icons/app-NAME.svg exists, otherwise
 * app-default. NAME is the last part of the path of the program, without
 * the "@" of a panel action. The panel loads the icons of its menu and of
 * its window buttons by this name, and the Open with chooser loads the
 * icons of its applications by it. */
#define LAUNCHER_ICON_DIR "/usr/share/icons"
const char *launcher_icon_name(const char *command, char *buf, int size);
