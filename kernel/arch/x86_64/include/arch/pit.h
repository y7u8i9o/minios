#pragma once
#include <kernel.h>

#define PIT_FREQUENCY 1193182u

/* Busy wait using PIT channel 2 in one shot mode. Usable before interrupts
 * are enabled, intended for timer calibration only. */
void pit_wait_us(unsigned us);
