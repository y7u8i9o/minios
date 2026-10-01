#include <arch/smp.h>

/* One processor until the application processors are started (A8). */

void smp_park_aps(void) {}
void smp_start_aps(void) {}
unsigned smp_cpu_count(void) { return 1; }
cpu_mask_t smp_online_mask(void) { return 1; }
bool smp_active(void) { return false; }
void smp_halt_others(void) {}
