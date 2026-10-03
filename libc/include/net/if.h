#pragma once
/* Network interface names and flags, for programs that include the
 * header. minios lists its interfaces in /dev/net and offers neither
 * SIOCGIFCONF nor getifaddrs. */
#define IF_NAMESIZE 16
#define IFNAMSIZ    IF_NAMESIZE

#define IFF_UP          0x1
#define IFF_BROADCAST   0x2
#define IFF_LOOPBACK    0x8
#define IFF_POINTOPOINT 0x10
#define IFF_RUNNING     0x40
#define IFF_MULTICAST   0x1000
