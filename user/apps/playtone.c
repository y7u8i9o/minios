/* Play a one-second 440 Hz triangle wave through audiod. */
#include <audio/audio.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    struct audio_connection *connection = audio_connect();
    if (!connection) {
        perror("playtone: audio_connect");
        return 1;
    }
    struct audio_playback *playback =
        audio_playback_create(connection, "playtone");
    if (!playback) {
        perror("playtone: create stream");
        audio_disconnect(connection);
        return 1;
    }
    uint32_t quantum = audio_playback_quantum(playback);
    int16_t *period = malloc((size_t)quantum * 2 * sizeof *period);
    if (!period) {
        perror("playtone: malloc");
        audio_disconnect(connection);
        return 1;
    }
    if (audio_playback_start(playback) < 0) {
        perror("playtone: start");
        free(period);
        audio_disconnect(connection);
        return 1;
    }

    uint32_t phase = 0;
    uint32_t periods = (48000 + quantum - 1) / quantum;
    uint32_t remaining = 48000;
    for (uint32_t p = 0; p < periods; p++) {
        uint32_t frames = remaining < quantum ? remaining : quantum;
        for (uint32_t i = 0; i < frames; i++) {
            uint32_t ramp = phase < 24000 ? phase : 48000 - phase;
            int16_t sample = (int16_t)((int32_t)ramp - 12000);
            period[i * 2] = sample;
            period[i * 2 + 1] = sample;
            phase += 440;
            if (phase >= 48000)
                phase -= 48000;
        }
        if (audio_playback_write(playback, period, frames) !=
            (ssize_t)frames) {
            perror("playtone: write");
            free(period);
            audio_disconnect(connection);
            return 1;
        }
        remaining -= frames;
    }
    int result = audio_playback_drain(playback);
    if (result < 0)
        perror("playtone: drain");
    free(period);
    audio_disconnect(connection);
    return result < 0;
}
