#pragma once
#include <sys/types.h>

#define WNOHANG 1
#define WUNTRACED 2
#define WCONTINUED 8
#define WEXITSTATUS(s)  (((s) >> 8) & 0xff)
#define WTERMSIG(s)     ((s) & 0x7f)
#define WSTOPSIG(s)     WEXITSTATUS(s)
#define WIFSTOPPED(s)   (((s) & 0xff) == 0x7f)
#define WIFCONTINUED(s) ((s) == 0xffff)
#define WIFEXITED(s)    (!WIFSTOPPED(s) && !WIFCONTINUED(s) && WTERMSIG(s) == 0)
#define WIFSIGNALED(s)  (!WIFSTOPPED(s) && !WIFCONTINUED(s) && WTERMSIG(s) != 0)

pid_t wait(int *status);
pid_t waitpid(pid_t pid, int *status, int options);
/* rusage, when not NULL, receives the child's consumption (struct rusage
 * from sys/resource.h). */
pid_t wait4(pid_t pid, int *status, int options, void *rusage);
