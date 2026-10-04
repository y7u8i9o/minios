/* flac.so: the Free Lossless Audio Codec (RFC 9639) in native FLAC
 * streams and in Ogg, decoded and encoded. */
#include "flac.h"

/* "fLaC" at the start, or after an ID3v2 tag that ends within the bytes
 * given to the probe. */
static int flac_probe(const uint8_t *data, size_t len)
{
    size_t at = flac_id3_length(data, len);
    return len >= at + 4 && memcmp(data + at, "fLaC", 4) == 0 ? 100 : 0;
}

static const struct codec flac_codecs[] = {
    {
        .name = "flac",
        .description = "Free Lossless Audio Codec",
        .kind = CODEC_AUDIO,
        .caps = CODEC_DECODE | CODEC_ENCODE,
        .mime_types = "audio/flac audio/x-flac",
        .extensions = "flac",
        .probe = flac_probe,
        .audio_open = flac_open,
        .audio_read = flac_read,
        .audio_close = flac_close,
        .audio_encode = flac_encode,
    },
    {
        /* Not audio/ogg, which belongs to Vorbis, the format of most .ogg
         * files. The probe finds FLAC in an Ogg file by its content. */
        .name = "oggflac",
        .description = "FLAC in Ogg",
        .kind = CODEC_AUDIO,
        .caps = CODEC_DECODE | CODEC_ENCODE,
        .mime_types = "audio/x-oggflac",
        .extensions = "oga",
        .probe = oggflac_probe,
        .audio_open = oggflac_open,
        .audio_read = oggflac_read,
        .audio_close = oggflac_close,
        .audio_encode = oggflac_encode,
    },
};

CODEC_MODULE(flac) = { CODEC_MODULE_ABI, "flac", 2, flac_codecs };
