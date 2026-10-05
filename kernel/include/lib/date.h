#pragma once
/* Conversions between calendar dates and day numbers of the proleptic
 * Gregorian calendar (kernel/lib/date.c). Day 0 is 1970-01-01. */
#include <kernel.h>

/* The day number of the date year-month-day, month 1 to 12. */
int64_t date_days_from_civil(int64_t year, int month, int day);
/* The date of a day number. */
void date_civil_from_days(int64_t days, int *year, int *month, int *day);
