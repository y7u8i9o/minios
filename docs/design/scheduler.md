# Scheduler

## FPU and SSE state (M23)

Every thread owns a 512 byte, 16 byte aligned `fxsave` area
(`thread->fpu`, initialised from a template made with `fninit` and
`MXCSR 0x1f80`). `sched_switch_locked` saves the outgoing thread's
registers with `fxsave64` before `context_switch` and restores the
incoming thread's after it; `thread_start` restores the area of a
thread on its first run. `fork` saves the parent's live registers into
the child's area, `execve` resets the area to the initial state, and
signal delivery copies the area into the signal frame so a handler may
clobber the registers freely (`sigreturn` restores them). Each CPU
enables `CR4.OSFXSR` and `CR4.OSXMMEXCPT` and clears `CR0.EM` and
`CR0.TS` in `paging_enable_features`. User programs are compiled with
SSE (`toolchain.mk`); the kernel remains built without it and never
touches the registers, so no save is needed on kernel entry.
