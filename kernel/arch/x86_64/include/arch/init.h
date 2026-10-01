#pragma once

/* Architecture steps of the start-up sequence (init/main.c), in the order
 * kmain calls them. The entry code (start.S) calls kmain on the boot stack
 * with interrupts disabled. */

/* Load the descriptor tables of the boot CPU and its struct cpu, so that
 * cpu_current() works. Runs before the kernel log ring is allocated. */
void arch_init_cpu_boot(void);
/* Install the exception and interrupt entry points. */
void arch_init_traps(void);
/* Identify the processor and log its features. */
void arch_init_cpu_features(void);
/* Initialize the interrupt controllers of the boot CPU. Runs after the
 * slab allocator, before the timer. */
void arch_init_interrupts(void);
