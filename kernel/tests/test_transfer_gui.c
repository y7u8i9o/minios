/* The window of the package transfer in the compositor (boot case
 * gui_transfer, docs/design/filetransfer.md). A transfer server of the
 * guest serves /tmp/served with one file on port 9102. The window starts with the argument
 * 127.0.0.1:9102, connects and lists the served folder. TRANSFER_LOG names
 * a file that receives the status lines of the window. The test waits for
 * the line of the connection, prints the file on the console for the
 * expect file of the case, and closes the window with Alt+F4. */
#include <tests/ktest.h>
#include <sched/proc.h>
#include <ipc/signal.h>
#include <drivers/timer.h>
#include <console.h>
#include "gui_helpers.h"

static void test_gui_transfer(void)
{
    install_app("transfer");
    run_shell("mkdir -p /tmp/served && echo 'served by the guest' > /tmp/served/served.txt");
    struct proc *server = proc_create_user("/usr/bin/transfer",
        (char *const[]){ "transfer", "serve", "-p", "9102", "/tmp/served", NULL },
        (char *const[]){ "HOME=/home", "PATH=/bin:/usr/bin", NULL }, &kernel_proc);
    ktest_assert(server != NULL, "cannot start the transfer server");
    struct proc *srv = start_server();
    struct proc *cl = proc_create_user("/usr/bin/transfer", (char *const[]){ "transfer", "127.0.0.1:9102", NULL },
        (char *const[]){ "HOME=/home", "PATH=/bin:/usr/bin", "TRANSFER_LOG=/tmp/transfer.log", NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start the transfer window");
    ktest_assert(wait_shell("grep -q '^listed 1 items in /$' /tmp/transfer.log", 60000),
                 "the window did not list the served folder");
    run_shell("cat /tmp/transfer.log");
    alt_key(0x3e);
    int status = proc_reap(cl);
    ktest_assert(status == 0, "transfer window status 0x%x", status);
    stop_server(srv);
    signal_send(server, SIGTERM);
    proc_reap(server);
    kprintf("gui_transfer: window ok\n");
}
KTEST_DEFINE("gui_transfer", test_gui_transfer);
