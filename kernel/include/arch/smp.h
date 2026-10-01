#pragma once
#include <kernel.h>
#include <cpu.h>

/* Symmetric multiprocessing (docs/design/arch.md), implemented by the
 * architecture. Application processors are enumerated and started through
 * the Limine MP protocol: the bootloader parks every AP on its own page
 * tables and jumps to the kernel entry when the kernel writes
 * goto_address. */

/* Take every AP off the bootloader's memory: load the kernel page tables,
 * a kernel stack and the descriptor or vector tables, then spin until
 * smp_start_aps. Must run
 * before bootloader reclaimable memory is freed. */
void smp_park_aps(void);
/* Release the parked APs into their per CPU initialization and idle loops.
 * Returns when every AP has started. */
void smp_start_aps(void);
/* Number of CPUs known to the kernel, including the boot CPU. */
unsigned smp_cpu_count(void);
/* Bit mask of CPUs that run kernel code. */
cpu_mask_t smp_online_mask(void);
/* True once the APs have been released. Cheap, read without a lock. */
bool smp_active(void);
/* Stop every other CPU. Used by panic. */
void smp_halt_others(void);
