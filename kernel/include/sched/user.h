#pragma once
#include <kernel.h>
#include <arch/trap.h>

struct proc;
struct thread;

/* Create a process from an ELF file on the initrd and make it runnable. */
struct proc *proc_create_user(const char *path, char *const argv[], char *const envp[],
                              struct proc *parent);
/* fork: duplicate the calling process; the child resumes from tf with
 * rax = 0. Returns the child or NULL. */
struct proc *proc_fork(struct trapframe *tf);
/* execve: replace the calling process image. On success tf is rewritten to
 * enter the new program. argv and envp are kernel copies. */
int proc_exec(struct trapframe *tf, const char *path, char *const argv[], char *const envp[]);
/* Create a user thread in the current process starting at entry with arg
 * in rdi and the given stack. Returns the tid. */
int user_thread_create(uintptr_t entry, uintptr_t arg, uintptr_t stack);
/* Kill the current user process because of a fault. */
__noreturn void user_fault_exit(int status);
