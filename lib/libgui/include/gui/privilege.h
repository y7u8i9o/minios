#pragma once
/* Commands run as root from the desktop (docs/design/users.md). sudo runs
 * them, and askpass, the authentication dialog of the desktop, asks for
 * the password of the user. */
#include <gui/app.h>
#include <stddef.h>
#include <sys/types.h>

#define ASKPASS_PATH "/usr/bin/askpass"

/* The state file of one run of sudo, shared by askpass and
 * app_run_privileged. askpass creates the file at its first prompt. A
 * second prompt of the same sudo finds the file and reports the wrong
 * password. askpass writes "cancel" into the file when the dialog is
 * cancelled. */
void askpass_state_path(char *buf, size_t size, uid_t uid, pid_t sudo_pid);

/* app_run_privileged runs argv as root with app_run_command. root runs
 * argv directly. Another user runs it through sudo -A, and the dialog
 * shows reason. out receives the output of sudo and of the command. The
 * result is that of app_run_command, or -ECANCELED when the dialog was
 * cancelled. */
int app_run_privileged(struct app *a, const char *reason, char *const argv[], const char *input, char *out,
                       size_t size);
