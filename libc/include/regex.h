#pragma once
#include <stddef.h>

typedef long regoff_t;
typedef struct {
    size_t re_nsub;
    void *__compiled;
    int __cflags;
} regex_t;

typedef struct {
    regoff_t rm_so;
    regoff_t rm_eo;
} regmatch_t;

#define REG_EXTENDED 0x0001
#define REG_ICASE    0x0002
#define REG_NOSUB    0x0004
#define REG_NEWLINE  0x0008

#define REG_NOTBOL   0x0010
#define REG_NOTEOL   0x0020
#define REG_STARTEND 0x0040

#define REG_NOMATCH  1
#define REG_BADPAT   2
#define REG_ECOLLATE 3
#define REG_ECTYPE   4
#define REG_EESCAPE  5
#define REG_ESUBREG  6
#define REG_EBRACK   7
#define REG_EPAREN   8
#define REG_EBRACE   9
#define REG_BADBR   10
#define REG_ERANGE  11
#define REG_ESPACE  12
#define REG_BADRPT  13
#define REG_EMPTY   14
#define REG_ESIZE   15

#define RE_DUP_MAX 255

int regcomp(regex_t *regex, const char *pattern, int flags);
int regexec(const regex_t *regex, const char *string, size_t nmatch,
            regmatch_t matches[], int flags);
size_t regerror(int error, const regex_t *regex, char *buffer, size_t size);
void regfree(regex_t *regex);
