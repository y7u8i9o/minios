#pragma once
#include <stdint.h>
#include <minios/abi.h>     /* struct timerfd_spec: initial_ms, interval_ms */

int timerfd_create(int flags);
int timerfd_settime(int fd, const struct timerfd_spec *spec);
int timerfd_gettime(int fd, struct timerfd_spec *spec);
