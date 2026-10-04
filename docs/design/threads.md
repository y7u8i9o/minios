# POSIX threads (M35)

User threads existed since M8 as a raw interface: `thread_create` with a
caller supplied stack, `thread_exit` and `thread_join`. M35 adds the three
things a threaded program needs on top of that and puts a POSIX interface
over them: thread local storage, a blocking primitive that user space can
build locks on, and a libc that is safe to call from several threads.

## Thread local storage

Every thread has a control block, `struct pthread` in `lib/libc/src/thread/tcb.h`,
whose address is the thread's FS base. The block starts with a pointer to
itself, so `%fs:0` yields the block and `pthread_self`, `errno` and the key
table are one load away. `errno` is a macro over `__errno_location()`,
which returns the `errno_value` field of the current block; there is no
global `errno` any more.

The kernel stores the base in `thread.arch.tls_base`. `set_tls(base)` stores
it and loads `MSR_FS_BASE` through `arch_set_tls`; every context switch
(`sched_switch_locked`) and every first run of a thread (`thread_start`)
load the MSR from the field of the thread being resumed
(`arch_thread_resume`), so the value is per thread on every CPU. `fork` copies
the parent's base to the child's thread, `exec` clears it (the new image
installs its own), and a raw `thread_create` inherits the creator's base so
that a thread created outside the pthread layer still finds a valid block
(the creator's: it shares that thread's `errno`).

The main thread's block is a static object installed by `__libc_start`
before anything else runs; its `tid` comes from the new `gettid` system
call. A pthread's block sits at the top of the thread's own anonymous
mapping, above its stack, and the thread entry installs it with `set_tls`
before calling the start routine.

The raw `thread_create` wrapper reserves a synthetic return-address slot
below the aligned stack top. The kernel jumps to the C entry function
without a call instruction, so this establishes the x86-64 ABI requirement
that RSP is 8 modulo 16 on entry. Without it, aligned SIMD spills inside
Lua can fault. Invalid, overflowing or insufficient stack ranges are rejected
before writing the slot. Raw entries must finish with `thread_exit`.

## Futexes

`futex(addr, op, value, timeout_ms)` in `kernel/ipc/futex.c` is the
sleeping half of every lock. `FUTEX_WAIT` sleeps while the 32-bit word at
`addr` still contains `value`, `FUTEX_WAKE` wakes up to `value` sleepers of
that word. A sleeper hashes (process, address) into one of 64 buckets,
links a record with its own wait queue there and blocks; the check of the
word happens under the bucket lock, so a wake issued after the caller's own
check cannot be lost. A wake walks the bucket, unlinks the records with the
same key and wakes each one. Timeouts use `waitq_wait_timeout`, and a
pending signal ends the wait with `EINTR`; spurious wakeups are allowed and
every caller re-checks its word. Futexes are private to a process: the key
includes the process, and a word in shared memory does not wake another
process.

## The pthread interface

`pthread.h` provides threads (`pthread_create`, `pthread_join`,
`pthread_detach`, `pthread_exit`, `pthread_self`, `pthread_equal`, attributes
for the stack size and the detach state), mutexes (normal, recursive and
error checking; `lock`, `trylock`, `timedlock`, `unlock`), condition
variables (`wait`, `timedwait` on `CLOCK_REALTIME`, `signal`, `broadcast`),
keys with destructors, `pthread_once`, spin locks and read-write locks.

A thread's stack and control block are one `mmap` of the attribute's stack
size (256 KiB by default) plus the block; `pthread_join` unmaps it after the
kernel reports the thread finished. A detached thread cannot unmap the stack
it runs on, so it marks itself exited and the next `pthread_create`,
`pthread_join` or `pthread_detach` reaps it: joins the kernel thread and
unmaps. The main thread's `pthread_exit` ends only that thread; the process
lives on until its last thread exits, as the kernel already did for raw
threads.

The mutex is the three state futex mutex: 0 free, 1 acquired, 2 acquired with a
possible waiter. An uncontended lock is one compare and exchange and an
uncontended unlock one exchange; the contended path marks the word with 2,
sleeps on it, and an unlock that finds a 2 wakes one sleeper. Recursive and
error checking mutexes remember their owner's `tid` and a depth. A
condition variable is a sequence number: a waiter records it, releases the
mutex, sleeps while the number is unchanged and takes the mutex back; a
signal increments the number and wakes one sleeper, a broadcast wakes all.
A signal between the release and the sleep changes the number, and the
futex wait then returns at once. Read-write locks are a mutex and two
conditions with writer preference; spin locks are a single exchanged word.

## libc under threads

`malloc`, `free` and `realloc` run under one lock, every `FILE` has its own
recursive lock (so `vfprintf` may call `fputc` on the stream it has acquired), and
`atexit` is locked. The open-stream registry has a separate `streams_lock`.
`fdopen` publishes under that lock, `fflush(NULL)` acquires it while walking
entries, and `fclose` unpublishes before flushing and freeing the object.
The registry lock nests outside each FILE lock, preventing a global flush
from retaining a stream that another thread frees. Callers still own the
responsibility for coordinating direct uses of the same FILE with fclose.
The lock, `struct __libc_lock`, is a recursive futex
mutex keyed by `tid`. Everything else in libc is reentrant or documented as
not thread safe in POSIX (`gmtime`, `asctime`, `strtok`).

## System calls added

`set_tls`, `gettid` and `futex`; the clock calls are described in
`docs/design/time.md`.

## Test

`pthreads` (`user/tests/pthreadtest.c`): eight threads incrementing a
counter under a mutex, producers and consumers on a bounded queue with
two condition variables, recursive and error checking mutexes, a timed
wait that expires, `errno` retained per thread across a blocking exchange with
another thread, keys with destructors, `pthread_once` from four threads,
detached threads with small stacks that are reclaimed, `malloc` and `printf`
from several threads, spin and read-write locks, aligned SIMD stack access,
invalid raw-stack rejection, and concurrent private-stream open/flush/close.

The [Lua thread binding](lua-threads.md) uses this pthread implementation.
Its `lua_threads` and `luasynth_worker` cases verify the language binding
and independent audio progress inside MiniOS.
