#pragma once
#include <netinet/in.h>

/* Options at the IPPROTO_TCP level, for programs that name them. The TCP
 * of minios stores no options at this level, and setsockopt fails for them
 * with ENOPROTOOPT. */
#define TCP_NODELAY 1
