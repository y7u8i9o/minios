#pragma once
#include <stdint.h>
#include <stddef.h>
#include <sys/cdefs.h>
typedef int pid_t;
typedef long ssize_t;
typedef long off_t;
typedef unsigned int mode_t;
typedef unsigned long ino_t;
typedef unsigned long dev_t;
typedef unsigned int uid_t;
typedef unsigned int gid_t;
typedef unsigned int id_t;              /* a pid, uid or gid, for getpriority */
typedef unsigned long nlink_t;
typedef long time_t;
typedef long blkcnt_t;
typedef long blksize_t;

/* BSD short names. */
typedef unsigned char u_char;
typedef unsigned short u_short;
typedef unsigned int u_int;
typedef unsigned long u_long;
typedef unsigned char uchar_t;
typedef unsigned short ushort_t;
typedef unsigned int uint_t;
typedef unsigned long ulong_t;
typedef long suseconds_t;
typedef unsigned int useconds_t;
