#include <tests/ktest.h>
#include <sched/user.h>
#include <sched/proc.h>
#include <drivers/timer.h>

/* The test of U5 runs su, doas and sudo on the console. root sets the passwords of root
 * and user and logs out. user then runs doas, su and sudo, each of which
 * asks for a password on the terminal, and doas checks a rule file that
 * denies. Password prompts discard the input typed ahead, as on Unix, and
 * every answer is therefore typed after a pause in which the prompt
 * appears. The
 * serial output is matched by the expect file of the privilege case, and
 * each result passes through sed to gain a tag that the echo of the typed
 * command does not contain. */
static void line_after(int ms, const char *text)
{
    sleep_ms(ms);
    type_line(text);
}

static void test_privilege(void)
{
    struct proc *p = proc_create_user("/bin/init", (char *const[]){ "/bin/init", NULL },
                                      (char *const[]){ "PATH=/bin", NULL }, &kernel_proc);
    ktest_assert(p != NULL, "cannot start /bin/init");
    proc_set_init(p);
    line_after(2500, "root\n");
    /* root has no password and chooses one at its first login. */
    line_after(1500, "rootpw\n");
    line_after(800, "rootpw\n");
    line_after(800, "printf 'rootpw\\nrootpw\\n' | passwd\n");
    line_after(800, "printf 'userpw\\nuserpw\\n' | passwd user\n");
    line_after(800, "printf 'deny user\\n' > /tmp/deny.conf\n");
    line_after(800, "exit\n");
    line_after(2000, "user\n");
    line_after(1500, "userpw\n");
    /* doas asks for the password of user, a member of wheel. */
    line_after(1500, "doas id -u | sed 's/^/doas=/'\n");
    line_after(1500, "userpw\n");
    line_after(1500, "doas -C /tmp/deny.conf true | sed 's/^/rule=/'\n");
    /* su asks for the password of root. */
    line_after(1500, "su\n");
    line_after(1500, "rootpw\n");
    line_after(1500, "id -u | sed 's/^/su=/'\n");
    line_after(800, "grep -c doas /var/log/messages | sed 's/^/logged=/'\n");
    line_after(800, "exit\n");
    /* sudo asks for the password of user, and -S reads it from a pipe. The
     * first run of sudo prints its lecture and creates the time stamp and
     * lecture records before the prompt, which takes several seconds
     * under TCG. */
    line_after(1500, "sudo id -un | sed 's/^/sudo=/'\n");
    line_after(6000, "userpw\n");
    line_after(3000, "echo wrongpw | sudo -S -k true\n");
    line_after(4000, "echo userpw | sudo -S -k id -gn | sed 's/^/group=/'\n");
    /* The command lines of the Users page of settings, where sudo reads
     * the first line and passwd the two that follow it. */
    line_after(2500, "printf 'userpw\\nbobpw\\nbobpw\\n' | sudo -S -k -p '' /bin/sh -c "
                     "\"useradd -c Bob bob && passwd bob\"\n");
    line_after(4000, "sudo -n grep -c '^bob:[^!*]' /etc/shadow | sed 's/^/added=/'\n");
    line_after(1500, "echo userpw | sudo -S -k -p '' /bin/sh -c 'userdel -r bob'\n");
    line_after(4000, "ls /home | sed 's/^/home=/'\n");
    line_after(2500, "initctl poweroff\n");
    int status = proc_reap(p);
    ktest_fail("init exited with status 0x%x", status);
}
KTEST_DEFINE("privilege", test_privilege);
