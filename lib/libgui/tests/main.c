/* Host unit tests of libgui: one binary, one function per area. */
#include "check.h"
#include <execinfo.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

static void on_crash(int sig)
{
    void *frames[32];
    int n = backtrace(frames, 32);
    backtrace_symbols_fd(frames, n, 2);
    _exit(139);
}

int failures, checks;
void run_framework_tests(void);
void run_image_tests(void);
void run_widget2_tests(void);
void run_gedit_tree_test(void);
void run_mime_tests(void);
void run_svg_tests(void);
void run_filechooser_tests(void);
void run_dnd_tests(void);
void run_appchooser_tests(void);
void run_resize_tests(void);
void run_pixel_tests(void);
void run_csd_tests(void);
void run_buffers_tests(void);

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);   /* failures before a crash remain visible */
    signal(SIGSEGV, on_crash);
    signal(SIGBUS, on_crash);
    signal(SIGALRM, on_crash);
    alarm(30);
    RUN(run_framework_tests);
    RUN(run_image_tests);
    RUN(run_widget2_tests);
    RUN(run_gedit_tree_test);
    RUN(run_mime_tests);
    RUN(run_svg_tests);
    RUN(run_filechooser_tests);
    RUN(run_dnd_tests);
    RUN(run_appchooser_tests);
    RUN(run_resize_tests);
    RUN(run_pixel_tests);
    RUN(run_csd_tests);
    RUN(run_buffers_tests);
    printf("libgui tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
