#include <tests/ktest.h>
#include <sched/user.h>
#include <sched/proc.h>
#include <drivers/timer.h>

/* U3: the console login. init runs login on the console. root sets its
 * password and creates the account anna with useradd and passwd, then logs
 * out. anna logs in with her password, finds herself in her home with the
 * terminal hers, is refused by doas and sudo because she is not a member
 * of wheel, and cannot read /etc/shadow. root removes the account again and
 * powers off. The serial output is matched by the expect file of the
 * login_console case. */
static void test_login(void)
{
    /* root has no password and chooses one at its first login. */
    type_line("root\n");
    type_line("rootpw\n");
    type_line("rootpw\n");
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
    type_line("doas -n true\n");
    type_line("echo annapw | sudo -S id\n");
    type_line("cat /etc/shadow\n");
    type_line("exit\n");
    type_line("root\n");
    type_line("rootpw\n");
    type_line("userdel -r anna\n");
    type_line("ls /home\n");
    type_line("initctl poweroff\n");
    struct proc *p = ktest_start_init();
    int status = proc_reap(p);
    ktest_fail("init exited with status 0x%x", status);
}
KTEST_DEFINE("login_console", test_login);

/* The graphical session from the console. user logs in, chooses a password
 * and runs startgui, which runs the display server as user on the display
 * that login gave to the account. The terminal of the session then powers
 * off the machine. The serial output is matched by the expect file of the
 * login_gui case. */
static void test_login_gui(void)
{
    struct proc *p = ktest_start_init();
    ktest_wait_idle(2500);
    type_line("user\n");
    ktest_wait_idle(1000);
    type_line("userpw\n");
    type_line("userpw\n");
    ktest_wait_idle(1500);
    type_line("ls -l /dev/fb0\n");
    ktest_wait_idle(800);
    type_line("startgui\n");
    ktest_wait_idle(8000);
    type_line("initctl poweroff\n");
    int status = proc_reap(p);
    ktest_fail("init exited with status 0x%x", status);
}
KTEST_DEFINE("login_gui", test_login_gui);
