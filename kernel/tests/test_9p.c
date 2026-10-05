/* V5 of docs/plan/release-0.6.0.md: init mounts the folder of the host at
 * /mnt/host during the boot (fsinit). tests/cases/9p_automount/mkshare
 * writes the file that the test reads. */
#include <tests/ktest.h>
#include <console.h>
#include <drivers/timer.h>
#include <fs/vfs.h>
#include <lib/string.h>
#include <sched/proc.h>
#include <errno.h>

struct proc *ktest_start_init(void);

static void test_9p_automount(void)
{
    ktest_start_init();
    struct file *f = NULL;
    int r = -ENOENT;
    for (int i = 0; i < 600 && r < 0; i++) {
        r = vfs_open("/mnt/host/hello.txt", O_RDONLY, 0, &f);
        if (r < 0)
            sleep_ms(100);
    }
    ktest_assert(r == 0, "no /mnt/host/hello.txt after 60 s: %d", r);
    char text[64];
    long n = file_read(f, text, sizeof text - 1);
    file_put(f);
    ktest_assert(n > 0, "read /mnt/host/hello.txt: %ld", n);
    text[n] = '\0';
    ktest_assert(strcmp(text, "mounted by fsinit\n") == 0, "/mnt/host/hello.txt contains %s", text);
    kprintf("9p_automount: /mnt/host/hello.txt: %s", text);
}
KTEST_DEFINE("9p_automount", test_9p_automount);
