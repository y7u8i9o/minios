/* cal [[month] year]: the calendar of a month, today marked. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *const names[12] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};

static int days_in(int month, int year)
{
    static const int days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return days[month] + (month == 1 && leap);
}

int main(int argc, char **argv)
{
    time_t now = time(NULL);
    struct tm today;
    gmtime_r(&now, &today);
    int month = today.tm_mon, year = today.tm_year + 1900;
    if (argc == 3) {
        month = atoi(argv[1]) - 1;
        year = atoi(argv[2]);
    } else if (argc == 2) {
        year = atoi(argv[1]);
        month = 0;
    }
    if (month < 0 || month > 11 || year < 1 || year > 9999) {
        fprintf(stderr, "usage: cal [[month] year]\n");
        return 1;
    }
    int months = argc == 2 ? 12 : 1;
    for (int m = month; m < month + months; m++) {
        struct tm first = { .tm_year = year - 1900, .tm_mon = m, .tm_mday = 1 };
        mktime(&first);
        char title[64];
        snprintf(title, sizeof title, "%s %d", names[m], year);
        int pad = (20 - (int)strlen(title)) / 2;
        printf("%*s%s\n", pad > 0 ? pad : 0, "", title);
        printf("Su Mo Tu We Th Fr Sa\n");
        int col = 0;
        for (int i = 0; i < first.tm_wday; i++, col++)
            printf("   ");
        for (int day = 1; day <= days_in(m, year); day++) {
            int mark = year == today.tm_year + 1900 && m == today.tm_mon && day == today.tm_mday;
            if (mark)
                printf(day < 10 ? "[%d]" : "%d]", day);
            else
                printf("%2d", day);
            col++;
            if (col == 7) {
                printf("\n");
                col = 0;
            } else {
                printf(" ");
            }
        }
        if (col)
            printf("\n");
        if (m + 1 < month + months)
            printf("\n");
    }
    return 0;
}
