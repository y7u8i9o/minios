#pragma once
#include <kernel.h>
#include <minios/abi.h>

struct proc;

/* Fill ru from the totals of p: its own consumption, or that of its reaped
 * children. Resident size is counted on demand for a live process. */
void rusage_of_proc(struct proc *p, struct rusage *ru, bool children);
