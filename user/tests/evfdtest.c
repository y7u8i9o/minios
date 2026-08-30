/* M23: eventfd, timerfd and poll timeouts. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <sys/ipc.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("evfdtest: FAIL " __VA_ARGS__); printf(" (errno %d)\n", errno); } } while (0)

int main(void)
{
    int e = eventfd(0, EFD_NONBLOCK);
    CHECK(e >= 0, "eventfd");
    uint64_t v = 99;
    errno = 0;
    CHECK(eventfd_read(e, &v) == -1 && errno == EAGAIN, "empty eventfd is not readable");
    CHECK(eventfd_write(e, 3) == 0 && eventfd_write(e, 4) == 0, "eventfd writes");
    struct pollfd p = { e, POLLIN, 0 };
    CHECK(poll(&p, 1, 0) == 1, "eventfd readable");
    CHECK(eventfd_read(e, &v) == 0 && v == 7, "eventfd counts writes: %lu", (unsigned long)v);
    CHECK(poll(&p, 1, 0) == 0, "eventfd drained");
    /* Poll timeout accuracy. */
    long t0 = uptime_ms();
    CHECK(poll(&p, 1, 100) == 0, "poll times out");
    long dt = uptime_ms() - t0;
    CHECK(dt >= 100 && dt < 140, "poll timeout took %ld ms", dt);
    /* Periodic timer: 20 ms period, count expirations over 210 ms. */
    int t = timerfd_create(TFD_NONBLOCK);
    CHECK(t >= 0, "timerfd_create");
    struct timerfd_spec spec = { 20, 20 };
    CHECK(timerfd_settime(t, &spec) == 0, "timerfd_settime");
    t0 = uptime_ms();
    uint64_t total = 0;
    while (uptime_ms() - t0 < 210) {
        struct pollfd tp = { t, POLLIN, 0 };
        if (poll(&tp, 1, 50) == 1) {
            uint64_t n;
            if (read(t, &n, 8) == 8)
                total += n;
        }
    }
    CHECK(total >= 8 && total <= 11, "periodic expirations in 210 ms: %lu", (unsigned long)total);
    struct timerfd_spec got;
    CHECK(timerfd_gettime(t, &got) == 0 && got.interval_ms == 20 && got.initial_ms <= 20, "timerfd_gettime %lu/%lu",
          (unsigned long)got.initial_ms, (unsigned long)got.interval_ms);
    /* One shot timer, blocking read. */
    int t2 = timerfd_create(0);
    struct timerfd_spec once = { 30, 0 };
    timerfd_settime(t2, &once);
    t0 = uptime_ms();
    uint64_t n = 0;
    CHECK(read(t2, &n, 8) == 8 && n == 1, "one shot expiration read once: %lu", (unsigned long)n);
    dt = uptime_ms() - t0;
    CHECK(dt >= 29 && dt < 60, "one shot after %ld ms", dt);
    struct pollfd t2p = { t2, POLLIN, 0 };
    CHECK(poll(&t2p, 1, 50) == 0, "one shot does not repeat");
    printf("evfdtest: %d failures\n", failures);
    return failures ? 1 : 0;
}
