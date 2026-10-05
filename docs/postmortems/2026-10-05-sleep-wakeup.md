# Postmortem: late and early ends of timed sleeps

| Field | Value |
| --- | --- |
| Document | PM-2026-10-05-01 |
| Version | 1.1 |
| Date | 2026-10-05 |
| Branch | `bleeding-edge` at commit `0a22a4a` |
| Status of the fix | Applied in the working tree, not committed |

## 1 Purpose

This document records two timing defects of timed sleeps. It gives the
symptoms, the causes, the corrections, the verification, and the lessons.

## 2 Audience

This document is for the developers of minios. You must know the MLFQ
scheduler of `kernel/sched/mlfq.c` and the boot test runner.

## 3 Summary

A timed sleep ended late whenever the CPUs ran busy threads. With one busy
process per CPU, a sleep of 10 ms overshot by 163 ms on average and by up
to 388 ms. The defect affected every timed sleep of the kernel
(`sleep_ms`) and of user programs (`sleep`, `usleep` and `nanosleep`, which
use the system call `sleep_ms`).

The cause was in `tick_local`. The tick moved the sleepers whose time had
come to the ready levels, but it requested a reschedule only on an idle
CPU. A woken sleeper therefore waited for the rest of the time slice of the
running thread. The slice of the lowest level is 1280 ms. The correction
requests a reschedule whenever the tick has woken a sleeper and the CPU
runs a thread other than its idle thread.

The new boot case `sleep_latency` measures the latency. It fails on the
old kernel and passes on the corrected kernel with an overshoot of about
1.2 ms.

A second defect ended timed sleeps too early. Every wake of a sleeping
thread ended its sleep, including a stale wake that was meant for an
earlier wait. `sleep_ms` returned without a check of the time, and the
system call returned 0. The new kernel test `sched_wake_race` forces stale
wakes: on the old kernel 2999 of 3000 sleeps of 1 ms ended early. The
correction repeats a sleep until its deadline, ends the sleep of a system
call only for a signal (`EINTR`), and routes a wake of a sleeper to the CPU
of its sleeper list.

## 4 Symptoms

- The luasynth investigation of 2026-09-10
  (`docs/design/luasynth-performance.md`) measured render calls of up to
  47 ms for a 10 ms audio quantum and left these delays unexplained. 47 ms
  is the 40 ms slice of level 2 plus the render time.
- Boot cases failed at random under parallel load and passed alone.
- An agent reported a timing problem of the whole system to the owner in
  an earlier conversation. That conversation was lost.

## 5 Timeline

- 2026-09-10: the luasynth investigation records long render delays as
  open work.
- 2026-10-05: the replacement of fixed waits in the kernel tests by
  `ktest_wait_idle` shows failures that move between cases from run to
  run. The owner recalls an earlier report of a timing problem of the whole
  system.
- 2026-10-05: the review of the tick finds the missing reschedule. The
  case `sleep_latency` confirms the defect, and the correction removes it.

## 6 Investigation

### 6.1 Wake-up paths

A thread leaves a timed sleep in one of two ways:

- `sched_wake` queues the thread in the inbox of its CPU through
  `queue_inbound`. `kick_cpu` sets `need_resched` on that CPU and sends a
  reschedule interrupt. The running thread yields at the next return to
  user mode.
- The tick of the CPU moves the sleeper from the sorted sleeper list to
  its ready level (`wake_sleepers_locked`). The tick set `need_resched`
  only when the CPU ran its idle thread.

The second path lacked the reschedule. A CPU that ran a busy thread
therefore continued that thread until its slice ended. The slice doubles
with every demotion, from 10 ms on level 0 to 1280 ms on level 7. The
priority boost resets the levels once per second.

### 6.2 Reproduction

`user/tests/sleeplattest.c` starts one busy process per CPU and lets them
run for one second, so that the scheduler demotes them. Then the program
sleeps 10 ms fifty times with `usleep` and measures each sleep with
`CLOCK_MONOTONIC`. The old kernel gave:

    sleeplattest: 4 busy processes, overshoot mean 162544 us, max 388416 us

### 6.3 Stale wakes

The review of the wake-up paths found a second race. `interrupt_threads` (`kernel/ipc/signal.c`)
calls `waitq_signal(t)` and then `sched_wake(t)`. The second call can read
the state `BLOCKED` before the inbox of the first wake is drained, and
take the claim after the drain released it. It then queues a stale wake
and overwrites `t->cpu`. When the thread sleeps later, a drain of the
stale wake removes it from a sleeper list, possibly the list of another
CPU, without the lock of that CPU. The race can explain the unresolved
panic `enqueue waiter x12` of the luasynth investigation. A diagnostic run
of 86 boot cases found eleven early ends of `sleep_ms`, all caused by
signals that the threads were meant to receive.

The kernel test `sched_wake_race` forces the pattern of
`interrupt_threads`. A victim thread blocks on a wait queue and then sleeps
1 ms, 3000 times. A waker calls `waitq_wake_one` and then `sched_wake` at
once, and two more threads call `sched_wake` on the victim without a pause.
On the old kernel 2999 of the 3000 sleeps ended before their deadline. A
diagnostic of the drain found no wake for a sleeper of another CPU in that
run. The correction covers that path as well, because the code permitted
it.

## 7 Causes

### 7.1 Root cause

`tick_local` in `kernel/sched/mlfq.c` set `need_resched` after waking
sleepers only on an idle CPU. A woken sleeper on a busy CPU waited for the
end of the running thread's slice.

The second root cause was `sleep_ms` in `kernel/drivers/timer.c`. It
called `sched_sleep_until` once and returned at the first wake. The system
call `sleep_ms` returned 0 after any wake, so `nanosleep`, `usleep` and
`sleep` reported a complete sleep.

### 7.2 Contributing causes

- No test measured the latency of a sleep beside busy threads. The case
  `sched` tests sleeps on otherwise idle CPUs.
- `interrupt_threads` wakes a thread twice, through its wait queue and
  through `sched_wake`. The second wake can arrive after the first one has
  been consumed.
- `queue_inbound` overwrites `t->cpu` with the target of the wake. A stale
  wake therefore could send a later wake of a sleeper to a CPU whose
  sleeper list does not contain the thread.
- `wake_sleepers_locked` stopped at the first sleeper whose wake was
  already queued, and the sleepers behind it waited for the next tick.

## 8 Corrections

- `wake_sleepers_locked` returns the number of woken sleepers. It skips a
  sleeper whose wake is queued and wakes the due sleepers behind it.
- `tick_local` sets `need_resched` when it has woken a sleeper and the CPU
  runs a thread other than its idle thread. The running thread yields at
  the next return to user mode and goes to the end of its level. The woken
  sleeper usually has the higher level and runs first.
- `sched_sleep_until(tick, interrupted)` records `sleep_cpu`, the CPU of
  the sleeper list, publishes `THREAD_SLEEPING` with sequential consistency
  and then calls `interrupted`. `send_info` publishes the pending signal
  with sequential consistency, so a signal either finds the sleeper or the
  sleeper finds the signal.
- `sched_wake` sends the wake of a sleeper to `sleep_cpu`. A drain that
  finds a sleeper of another CPU forwards the wake to that CPU instead of
  changing a foreign list.
- `sleep_ms` sleeps again until the deadline after an early wake.
  `sleep_ms_interruptible` also ends at `signal_should_interrupt` and
  returns `-EINTR`. The system call `sleep_ms` uses it, as the other
  blocking calls do.
- `nanosleep` in the libc reports the remaining time after `EINTR`, and
  `sleep` returns the unslept seconds, as POSIX requires.
- `sys_reboot` waits 40 times 50 ms for the processes to end after
  `SIGTERM`. Every `SIGCHLD` of init used to end one of these sleeps early,
  which shortened the grace period. The period now lasts its full time.

## 9 Verification

### 9.1 Regression test

The boot case `sleep_latency` runs `sleeplattest` and requires a maximum
overshoot below 15 ms. On the corrected kernel:

    sleeplattest: 4 busy processes, overshoot mean 1185 us, max 1297 us

The case also checks the signals: a handled signal ends a 500 ms
`nanosleep` with `EINTR` and the remaining time, an ignored signal does not
shorten a 200 ms `usleep`, and `sleep(3)` interrupted after 100 ms returns
3.

### 9.2 Stale wakes

The kernel test `sched_wake_race` requires that all 3000 sleeps of the
victim last their full time.

### 9.3 Boot cases

187 boot cases ran on x86_64 with all corrections: the scheduler, signal,
thread and SMP cases, the libc, shell and shutdown cases, every network,
USB, AHCI, NVMe, mfs, audio, luasynth, input method, compositor and GUI
case, `sleep_latency` and `sched_wake_race`. 185 passed and 2 were skipped
as cases of aarch64 only. Those two, `ahci_aarch64` and `usb_hid_acpi`,
passed on aarch64. `gui_sysinfo` passes since its expectation and the
category table of `sysinfo` contain the categories `nvme` and `ahci` of the
drivers of R2 and R3.

## 10 Lessons

- Every path that makes a thread runnable must request a reschedule on
  the CPU of the thread. A review of a new wake-up path checks this rule.
- A latency test beside busy threads belongs to every scheduler change.
- A measurement that leaves a delay unexplained, such as the 47 ms render
  calls, is recorded as an open defect, not only as open work.

## 11 References

- `kernel/sched/mlfq.c`: `wake_sleepers_locked`, `tick_local`,
  `queue_inbound`, `kick_cpu`, `drain_inbound_locked`
- `kernel/ipc/signal.c`: `interrupt_threads`
- `kernel/drivers/timer.c`: `sleep_ms`, `sleep_ms_interruptible`
- `kernel/syscall/sys_misc.c`: `sys_sleep_ms`
- `lib/libc/src/time/time.c`: `nanosleep`; `lib/libc/src/unistd/unistd.c`: `sleep`
- `user/tests/sleeplattest.c`, `tests/cases/sleep_latency/`
- `kernel/tests/test_sched_wake.c`, `tests/cases/sched_wake_race/`
- `docs/design/sched.md`, `docs/design/luasynth-performance.md`

## 12 Glossary

- Overshoot: the measured length of a sleep minus the requested length.
- Slice: the time a thread may run before the scheduler demotes it and
  selects another thread.
- Stale wake: a wake notification in an inbox for a thread that no longer
  waits for it.
