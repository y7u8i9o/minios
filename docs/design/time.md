# Time (M35)

Before M35 the only clock visible to programs was `uptime_ms`, the
millisecond count of the TSC based timer. M35 adds a real time clock, two
POSIX clocks and the `time.h` family in libc.

## The real time clock

`kernel/drivers/rtc.c` reads the CMOS clock once at boot, right after
`timer_init`: it waits for the update flag to clear, reads the registers
twice until they agree, converts BCD and 12 hour values as the status
register says, and turns the calendar date into seconds since the Unix
epoch (QEMU keeps the CMOS clock in UTC). The difference between that
instant and the timer's own count becomes the epoch offset; the clock is
never read again, so the slow CMOS port access cannot disturb a running
system. The boot log records the date (`rtc: 2026-09-03 15:16:57 UTC`), and
an implausible clock (before 2000) falls back to 2000-01-01 with a warning.

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
rounded up, over `sleep_ms`), `gmtime`, `gmtime_r`, `localtime` (UTC: the
system has no time zones), `timegm`, `mktime` (normalizing the fields, so
December 32nd becomes January 1st), `strftime` with the common conversions,
`asctime` and `ctime`. `sys/time.h` adds `gettimeofday` and
`settimeofday`. The calendar arithmetic uses the era based civil date
algorithms, which are exact for every year of the proleptic Gregorian
calendar in both directions.

## Programs

`date` prints the time (`Thu Sep  3 15:16:57 UTC 2026`), takes a `+FORMAT`
argument for `strftime`, and sets the clock with `-s "YYYY-MM-DD HH:MM:SS"`.
`cal` prints the calendar of the current month with today marked, or of a
given month and year, or of a whole year. The panel's clock and the clock
application show the time of day.

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
