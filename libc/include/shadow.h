#pragma once
#include <stdio.h>

/* The password hashes of /etc/shadow, readable by root only, one per
 * line: name:hash:lastchange:min:max:warn:inactive:expire:flag. An empty
 * hash means no password, and a hash starting with "!" or "*" a locked
 * account. Only the name and the hash are used by minios. Empty numeric
 * fields read as -1. */
struct spwd {
    char *sp_namp;
    char *sp_pwdp;
    long sp_lstchg;
    long sp_min;
    long sp_max;
    long sp_warn;
    long sp_inact;
    long sp_expire;
    unsigned long sp_flag;
};

struct spwd *getspnam(const char *name);
struct spwd *fgetspent(FILE *stream);
