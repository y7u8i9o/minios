# User mode and system calls (M8)

## Entry and exit

`syscall_init` programs `STAR` so that `syscall` loads CS `0x08` and SS
`0x10`, and `sysret` loads CS `0x20|3` and SS `0x18|3`, matching the GDT.
`LSTAR` points at `syscall_entry` and `SFMASK` clears IF, TF, DF and AC on
entry. `EFER.SCE` enables the instructions.

`arch/x86_64/syscall.S` swaps GS, saves the user stack pointer in
`cpu->user_rsp`, loads `cpu->kstack_top`, and pushes SS, RSP, RFLAGS (from
r11), CS, RIP (from rcx), a zero error code, a pseudo vector 256 and all
general purpose registers, producing a `struct trapframe` identical to the
interrupt stubs' layout. `syscall_dispatch` enables interrupts, looks up the
number in `rax`, stores the result in the frame's `rax`, disables interrupts
again and runs the preemption point. The exit path restores the registers,
reloads rcx and r11 from the frame, swaps GS, loads the user stack and
executes `sysretq`.

Interrupts and exceptions from ring 3 go through the same stubs as before:
`isr_common` checks the saved CS and executes `swapgs` on entry and before
`iretq`. `user_enter(tf)` points `rsp` at a frame and jumps to the return
path; it is how a new user thread first reaches ring 3.

## Preemption

The kernel is not preemptible. The timer marks a reschedule request and it
is honoured in `trap_dispatch` after an interrupt from user mode and in
`syscall_dispatch` before returning, both with interrupts disabled. Kernel
threads keep yielding voluntarily.

## Processes

`proc_create_user` allocates a process and address space, maps zeroed pages
for the image at `0x400000` (read, write, execute) and four stack pages
below `0x7ffffffff000`, copies the image through the direct map, and builds
an entry frame with user selectors, `RFLAGS.IF` set and `rip` at the load
address. The thread starts in `user_thread_entry`, which copies the frame
to its kernel stack and calls `user_enter`.

Exited user threads stay on `proc->zombies`. `proc_reap` waits for the
process to become a zombie, waits for each thread to have switched away,
frees them, releases every user frame with `vmspace_free_user_pages` and
frees the process. Kernel threads switch address spaces lazily, so
`vmspace_destroy` loads the kernel space first if the one being destroyed
is still active.

A fault in ring 3 kills the process with status `128 + vector` instead of
panicking. Faulting kernel code still panics.

## System calls

Numbers live in `kernel/include/syscall_nums.h`, shared with libc:
`write` (1), `exit` (2), `getpid` (3), `yield` (4). Arguments arrive in
rdi, rsi, rdx, r10, r8, r9. `write` accepts descriptors 1 and 2 only until
the VFS exists and checks that the buffer lies in user space with
`user_range_ok`. An address that is in range but unmapped still faults in
the kernel; proper fault recovery for user copies arrives with the VMA
layer in M9 to M11.

## First user program

`user/hello/hello.S` is assembled and linked as a flat binary at `0x400000`
by the kernel Makefile and embedded in `.rodata` between
`user_hello_start` and `user_hello_end`. It writes a message, calls
`getpid` and `yield`, prints the pid and exits with status 42. `kinit`
starts it after the self tests. M9 replaces the embedded binary with ELF
loading from an initrd.

## Test

`tests/cases/user` runs the embedded program and checks the status, runs a
program that dereferences address 0 and one that reads a kernel address
(both must die with status 142), runs four copies concurrently, and checks
that no physical pages leaked.
