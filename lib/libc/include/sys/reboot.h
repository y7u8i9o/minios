#pragma once
#include <minios/abi.h>

/* Only init (pid 1) may call this. Other programs signal init: SIGUSR1
 * for power off, SIGUSR2 for reboot. */
int reboot(int cmd);
