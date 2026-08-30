#pragma once
#include <minios/abi.h>

/* Fill buf with the system identification. Returns 0. */
int uname(struct utsname *buf);
