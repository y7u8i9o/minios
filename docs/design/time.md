# Time (M35, L2)

Before M35 the only clock visible to programs was `uptime_ms`, the
millisecond count of the TSC based timer. M35 adds a real time clock, two
POSIX clocks and the `time.h` family in libc.

## The real time clock

`kernel/drivers/rtc.c` reads the clock once at boot, right after
`timer_init`, through `platform_rtc_read`. On the PC that function
(`kernel/arch/x86_64/cmos.c`) waits for the update flag of the CMOS clock
to clear, reads the registers twice until they agree, and converts BCD and
12 hour values as the status register says. `rtc_init` turns the calendar
date into seconds since the Unix epoch (QEMU sets the CMOS clock to UTC).
The difference between that instant and the timer's own count becomes the
epoch offset. Because `rtc_init` reads the clock only once, the slow CMOS
port access does not delay a running system. The boot log records the date
(`rtc: 2026-09-03 15:16:57 UTC`), and an implausible clock (before 2000)
falls back to 2000-01-01 with a warning.

## Clocks

`clock_gettime(clock, ts)` returns `CLOCK_MONOTONIC` as the timer's
nanoseconds since boot (`timer_ns`, the TSC scaled exactly) and
`CLOCK_REALTIME` as that plus the epoch offset. `clock_settime` accepts
`CLOCK_REALTIME` and replaces the offset; it does not write the CMOS clock,
so the change lasts until the next boot. The offset is one 64-bit word
written at boot and by `clock_settime` and read by `clock_gettime`.

## libc

`time.h` provides `clock_gettime`, `clock_settime`, `clock_getres`, `time`,
`clock` (the monotonic clock in microseconds: the kernel does not account
processor time per process), `difftime`, `nanosleep` (whole milliseconds,
rounded up, over `sleep_ms`), `gmtime`, `gmtime_r`, `localtime` in the
local zone described below, `timegm`, `mktime` (normalizing the fields, so
December 32nd becomes January 1st), `strftime` with the common conversions,
`asctime` and `ctime`. `sys/time.h` adds `gettimeofday` and
`settimeofday`. The calendar arithmetic uses the era based civil date
algorithms, which are exact for every year of the proleptic Gregorian
calendar in both directions.

## Time zones (L2)

`lib/libc/src/time/tz.c` converts times into the local zone. The zone is the
value of `TZ`, or the file `/etc/localtime` when `TZ` is not set, or UTC
when neither exists. A value of `TZ` that is a valid POSIX rule, such as
`CET-1CEST,M3.5.0,M10.5.0/3`, is that rule. Any other value names a zone
file relative to `/usr/share/zoneinfo` or by an absolute path, and a
leading colon always names a file. An unknown value selects UTC.

A rule consists of a standard name and offset and an optional daylight
saving name, offset and pair of dates. Names have at least three letters
or are quoted with angle brackets, such as `<+0545>`. Offsets are
`[+-]hh[:mm[:ss]]` west of UTC, and the daylight saving offset defaults to
one hour less. The dates have the forms `Jn` (day 1 to 365 without
February 29), `n` (day 0 to 365) and `Mm.w.d` (day `d` of week `w` of month
`m`, where week 5 is the last), each with an optional `/time` from -167 to
167 hours, 02:00 by default. A rule with a daylight saving name and no
dates takes the dates of the United States, `M3.2.0,M11.1.0`. When the end
date comes before the start date in the year, as in the southern
hemisphere and in the winter time of Europe/Dublin, daylight saving time is
the part of the year outside the interval.

A zone file is a TZif file of version 1, 2 or 3 (RFC 8536). The reader
takes the 64-bit data of version 2 and later, the transitions, the local
time types and the rule of the footer. A time before the first transition
takes the first standard type, a time before the last transition the type
of the transition before it, and a later time the rule of the footer, or
the last type when the file has no footer.

`tools/genzoneinfo.py` writes the 61 zone files of `user/share/zoneinfo`
and the list `zones.tab` of zone names and rules, which the build copies to
`/usr/share/zoneinfo`. The rules are those of the footers of the IANA time
zone database 2025b. Each file has no transitions, one local time type and
the rule as the footer. The C library therefore converts every date of a
zone with its current rule, also dates before the last change of the rule.

`tzset`, `localtime` and `mktime` read the zone again when `TZ` changes, or,
without `TZ`, when `stat` of `/etc/localtime` returns another device,
inode or modification time. A program therefore follows a replaced
symbolic link at its next conversion. `tzset` sets `tzname` to the
standard and daylight saving names, `timezone` to the standard offset in
seconds west of UTC and `daylight` to 1 when the zone has daylight saving
time. A lock protects the zone state.

`localtime` adds the offset of the instant and sets `tm_isdst`. `mktime`
converts the fields as a local time. A time that occurs twice when the
clocks go back takes the offset that `tm_isdst` names when it is 0 or
positive. A time that does not occur when the clocks go forward, such as
02:30 on the last Sunday of March in Paris, is converted with the offset
before the change, and the normalized result is 03:30. `strftime` takes
`%z` and `%Z` from the zone at the instant of its `struct tm`, which it
computes with `mktime`, because `struct tm` has no offset and no zone
name.

## Programs

`date` prints the local time in the format `date_fmt` of LC_TIME
(`Thu Sep  3 15:16:57 UTC 2026` in the C locale), takes a `+FORMAT`
argument for `strftime`, prints the time in UTC with `-u`, and sets the
clock with `-s "YYYY-MM-DD HH:MM:SS"` in local time. `cal` prints the
calendar of the current month with today marked, or of a given month and
year, or of a whole year. The panel's clock, the clock program, `ls`,
Files, the shell prompt, the Settings date page and the names of
screenshots use local time.

## Test

`time` (`user/tests/timetest.c`): the monotonic clock agrees with
`uptime_ms`, the real time clock is after 2025 (the RTC follows the host),
a 120 ms `nanosleep` advances both clocks by that much, `time` and
`gettimeofday` agree with the clock, known dates convert in both
directions (the epoch, the day before it, 2001-09-09 01:46:40, a leap day,
the end of 2099), `mktime` normalizes overflowing fields, `strftime`,
`asctime` and `ctime` produce the expected strings and `strftime` reports
an overflow, and `clock_settime` moves the real time clock without touching
the monotonic one. The case also expects the boot log's `rtc` line.

`timezone` (`user/tests/tztest.c`): the changes of the clocks in Paris and
New York in both directions, Sydney in January and July, quoted names and
offsets with minutes, `mktime` in the spring gap and for both occurrences
of 02:30 in October, the zone files of Shanghai, Tokyo, Paris, New York,
Tehran and Dublin, an unknown zone, `/etc/localtime` as a symbolic link
that is replaced while the program runs, `TZ` against `/etc/localtime`,
`date -u` and `date` in Tokyo, and Lua's `os.date` in Moscow.
