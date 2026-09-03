/* Raw virtio-snd PCM playback and device ABI test. */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/audio.h>
#include <sys/ipc.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("audiotest: FAIL " __VA_ARGS__); printf(" (errno %d)\n", errno); } } while (0)

static void make_period(int16_t *samples, uint32_t frames, uint32_t *phase)
{
    for (uint32_t i = 0; i < frames; i++) {
        /* A deterministic 440 Hz square wave, duplicated to stereo. */
        int16_t v = *phase < 24000 ? 10000 : -10000;
        samples[i * 2] = v;
        samples[i * 2 + 1] = v;
        *phase += 440;
        if (*phase >= 48000)
            *phase -= 48000;
    }
}

int main(void)
{
    int fd = open("/dev/pcm0", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    CHECK(fd >= 0, "open pcm0");
    if (fd < 0) {
        printf("audiotest: %d failures\n", failures);
        return 1;
    }
    errno = 0;
    int second = open("/dev/pcm0", O_WRONLY | O_NONBLOCK);
    CHECK(second < 0 && errno == EBUSY, "exclusive open");
    if (second >= 0)
        close(second);

    struct audio_info info;
    CHECK(ioctl(fd, AUDIO_GET_INFO, &info) == 0, "get info");
    CHECK(info.abi_version == AUDIO_ABI_VERSION &&
          (info.capabilities & AUDIO_CAP_PLAYBACK) &&
          (info.formats & AUDIO_FORMAT_S16_LE) &&
          (info.rates & AUDIO_RATE_48000), "reported capabilities");

    struct audio_params bad = {
        AUDIO_FORMAT_S16_LE, 44100, 2, 480, 4
    };
    errno = 0;
    CHECK(ioctl(fd, AUDIO_SET_PARAMS, &bad) < 0 && errno == EINVAL,
          "reject unsupported rate");

    struct audio_params params = {
        AUDIO_FORMAT_S16_LE, 48000, 2, 480, 4
    };
    CHECK(ioctl(fd, AUDIO_SET_PARAMS, &params) == 0, "set params");
    CHECK(ioctl(fd, AUDIO_PREPARE) == 0, "prepare");

    size_t bytes = params.period_frames * params.channels * sizeof(int16_t);
    int16_t *period = malloc(bytes);
    CHECK(period != NULL, "allocate period");
    uint32_t phase = 0;
    for (uint32_t i = 0; i < params.periods; i++) {
        make_period(period, params.period_frames, &phase);
        CHECK(write(fd, period, bytes) == (ssize_t)bytes, "queue initial period %u", i);
    }
    struct pollfd pf = { fd, POLLOUT, 0 };
    CHECK(poll(&pf, 1, 0) == 0, "full ring is not writable");
    CHECK(ioctl(fd, AUDIO_START) == 0, "start");

    const uint32_t total_periods = 24;
    for (uint32_t i = params.periods; i < total_periods; i++) {
        pf.revents = 0;
        CHECK(poll(&pf, 1, 1000) == 1 && (pf.revents & POLLOUT),
              "period completion %u", i);
        make_period(period, params.period_frames, &phase);
        CHECK(write(fd, period, bytes) == (ssize_t)bytes, "queue period %u", i);
    }
    CHECK(ioctl(fd, AUDIO_DRAIN) == 0, "drain");
    struct audio_status status;
    CHECK(ioctl(fd, AUDIO_GET_STATUS, &status) == 0, "get status");
    CHECK(status.state == AUDIO_STATE_OPEN && status.queued_frames == 0,
          "stopped state %u queued %u", status.state, status.queued_frames);
    CHECK(status.played_frames == (uint64_t)total_periods * params.period_frames,
          "played frames %lu", (unsigned long)status.played_frames);
    CHECK(status.last_error == 0, "last error %d", status.last_error);
    /* The capture side under a host backend without a capture voice (the
     * wav backend of this test): periods never arrive, and dropping the
     * stream returns promptly because the release hands them back. */
    if (info.capabilities & AUDIO_CAP_CAPTURE) {
        CHECK(ioctl(fd, AUDIO_SET_CAPTURE_PARAMS, &params) == 0, "set capture params");
        CHECK(ioctl(fd, AUDIO_CAPTURE_PREPARE) == 0, "capture prepare");
        CHECK(ioctl(fd, AUDIO_CAPTURE_START) == 0, "capture start");
        pf.events = POLLIN;
        pf.revents = 0;
        int ready = poll(&pf, 1, 100);
        CHECK(ready == 0 || (pf.revents & POLLIN), "capture poll %d", ready);
        CHECK(ioctl(fd, AUDIO_CAPTURE_DROP) == 0, "capture drop");
        CHECK(ioctl(fd, AUDIO_GET_CAPTURE_STATUS, &status) == 0 &&
              status.state == AUDIO_STATE_OPEN, "capture stopped %u", status.state);
        printf("audiotest: capture dropped after %lu frames\n", (unsigned long)status.played_frames);
    }
    free(period);
    close(fd);
    printf("audiotest: %d failures\n", failures);
    return failures ? 1 : 0;
}
