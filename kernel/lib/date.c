/* Calendar dates and day numbers of the proleptic Gregorian calendar.
 * The algorithms count in eras of 400 years, which repeat exactly, and in
 * years that start on March 1, which puts the leap day at the end of a
 * year. */
#include <lib/date.h>

int64_t date_days_from_civil(int64_t year, int month, int day)
{
    year -= month <= 2;
    int64_t era = (year >= 0 ? year : year - 399) / 400;
    int64_t yoe = year - era * 400;
    int64_t doy = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void date_civil_from_days(int64_t days, int *year, int *month, int *day)
{
    days += 719468;
    int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    int64_t doe = days - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *day = (int)(doy - (153 * mp + 2) / 5 + 1);
    *month = (int)(mp < 10 ? mp + 3 : mp - 9);
    *year = (int)(yoe + era * 400 + (*month <= 2));
}
