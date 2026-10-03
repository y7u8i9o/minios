#include <tests/ktest.h>
#include <sched/user.h>
#include <sched/proc.h>

/* U3: the console login. init runs login on the console. root sets its
 * password and creates the account anna with useradd and passwd, then logs
 * out. anna logs in with her password, finds herself in her home with the
 * terminal hers, fails su with a wrong password and succeeds with the right
 * one, and cannot read /etc/shadow. root removes the account again and
 * powers off. The serial output is matched by the expect file of the
 * login_console case. */
static void test_login(void)
{
    type_line("root\n");
    type_line("printf 'rootpw\\nrootpw\\n' | passwd\n");
    type_line("useradd -c 'Anna Example' anna\n");
    type_line("printf 'annapw\\nannapw\\n' | passwd anna\n");
    type_line("ls -ld /home/anna\n");
    type_line("exit\n");
    type_line("anna\n");
    type_line("wrongpw\n");
    type_line("anna\n");
    type_line("annapw\n");
    type_line("id\n");
    type_line("pwd\n");
    type_line("ls -l /dev/console\n");
    type_line("echo wrongpw | su -c id\n");
    type_line("echo rootpw | su -c id\n");
    type_line("cat /etc/shadow\n");
    type_line("exit\n");
    type_line("root\n");
    type_line("rootpw\n");
    type_line("userdel -r anna\n");
    type_line("ls /home\n");
    type_line("initctl poweroff\n");
    struct proc *p = proc_create_user("/bin/init", (char *const[]){ "/bin/init", NULL },
                                      (char *const[]){ "PATH=/bin", NULL }, &kernel_proc);
    ktest_assert(p != NULL, "cannot start /bin/init");
    proc_set_init(p);
    int status = proc_reap(p);
    ktest_fail("init exited with status 0x%x", status);
}
KTEST_DEFINE("login_console", test_login);
