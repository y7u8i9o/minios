#pragma once
#include <wchar.h>

typedef unsigned long wctype_t;

/* Character classes for the C locale extended to the Latin-1 supplement,
 * Latin Extended-A and B, Greek and Cyrillic; every other code point above
 * 0x9f is printable and neither alphabetic nor a digit. */
int iswalnum(wint_t c);
int iswalpha(wint_t c);
int iswblank(wint_t c);
int iswcntrl(wint_t c);
int iswdigit(wint_t c);
int iswgraph(wint_t c);
int iswlower(wint_t c);
int iswprint(wint_t c);
int iswpunct(wint_t c);
int iswspace(wint_t c);
int iswupper(wint_t c);
int iswxdigit(wint_t c);
wint_t towlower(wint_t c);
wint_t towupper(wint_t c);
wctype_t wctype(const char *name);
int iswctype(wint_t c, wctype_t type);
