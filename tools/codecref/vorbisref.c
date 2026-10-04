/* The reference Vorbis encoder and decoder of the host (libvorbis), used
 * by tools/gen_codec_fixtures.py to make the Vorbis fixtures and the
 * samples the decoder of minios is compared with.
 *
 *   vorbisref encode IN.wav OUT.ogg QUALITY SERIAL
 *   vorbisref decode IN.ogg OUT.raw
 *
 * encode reads a 16 bit PCM WAV file. decode writes signed 16 bit little
 * endian samples, rounded from the floats of libvorbisfile. Both convert
 * between the channel order of WAV files and that of Vorbis. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vorbis/vorbisenc.h>
#include <vorbis/vorbisfile.h>

/* WAV position to Vorbis channel, as in lib/libcodec/modules/vorbis/decode.c. */
static const int maps[9][8] = {
    { 0 }, { 0 }, { 0, 1 }, { 0, 2, 1 }, { 0, 1, 2, 3 }, { 0, 2, 1, 3, 4 }, { 0, 2, 1, 5, 3, 4 },
    { 0, 2, 1, 6, 5, 3, 4 }, { 0, 2, 1, 7, 5, 6, 3, 4 },
};

static int wav_channel(int channels, int vorbis_channel)
{
    for (int k = 0; k < channels && channels <= 8; k++)
        if (maps[channels][k] == vorbis_channel)
            return k;
    return vorbis_channel;
}

static unsigned le(const unsigned char *p, int n)
{
    unsigned v = 0;
    for (int i = n - 1; i >= 0; i--)
        v = v << 8 | p[i];
    return v;
}

static int encode(const char *in, const char *out, float quality, int serial)
{
    FILE *f = fopen(in, "rb");
    if (!f)
        return 1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *w = malloc(size);
    if (fread(w, 1, size, f) != (size_t)size)
        return 1;
    fclose(f);
    int channels = 0, rate = 0;
    const unsigned char *data = NULL;
    long data_len = 0;
    for (long at = 12; at + 8 <= size;) {
        long n = le(w + at + 4, 4);
        if (!memcmp(w + at, "fmt ", 4)) {
            channels = le(w + at + 10, 2);
            rate = le(w + at + 12, 4);
        } else if (!memcmp(w + at, "data", 4)) {
            data = w + at + 8;
            data_len = n;
        }
        at += 8 + n + (n & 1);
    }
    long frames = data_len / (2 * channels);
    vorbis_info vi;
    vorbis_info_init(&vi);
    if (vorbis_encode_init_vbr(&vi, channels, rate, quality))
        return 1;
    vorbis_comment vc;
    vorbis_comment_init(&vc);
    vorbis_comment_add_tag(&vc, "ENCODER", "minios gen_codec_fixtures");
    vorbis_dsp_state vd;
    vorbis_block vb;
    vorbis_analysis_init(&vd, &vi);
    vorbis_block_init(&vd, &vb);
    ogg_stream_state os;
    ogg_stream_init(&os, serial);
    ogg_packet h0, h1, h2;
    vorbis_analysis_headerout(&vd, &vc, &h0, &h1, &h2);
    ogg_stream_packetin(&os, &h0);
    ogg_stream_packetin(&os, &h1);
    ogg_stream_packetin(&os, &h2);
    FILE *o = fopen(out, "wb");
    ogg_page og;
    while (ogg_stream_flush(&os, &og)) {
        fwrite(og.header, 1, og.header_len, o);
        fwrite(og.body, 1, og.body_len, o);
    }
    long at = 0;
    int eos = 0;
    while (!eos) {
        long n = frames - at < 1024 ? frames - at : 1024;
        if (n > 0) {
            float **buf = vorbis_analysis_buffer(&vd, (int)n);
            for (long i = 0; i < n; i++)
                for (int c = 0; c < channels; c++) {
                    int k = wav_channel(channels, c);
                    int16_t s = (int16_t)le(data + ((at + i) * channels + k) * 2, 2);
                    buf[c][i] = s / 32768.0f;
                }
            vorbis_analysis_wrote(&vd, (int)n);
            at += n;
        } else {
            vorbis_analysis_wrote(&vd, 0);
        }
        while (vorbis_analysis_blockout(&vd, &vb) == 1) {
            vorbis_analysis(&vb, NULL);
            vorbis_bitrate_addblock(&vb);
            ogg_packet op;
            while (vorbis_bitrate_flushpacket(&vd, &op)) {
                ogg_stream_packetin(&os, &op);
                while (ogg_stream_pageout(&os, &og)) {
                    fwrite(og.header, 1, og.header_len, o);
                    fwrite(og.body, 1, og.body_len, o);
                    if (ogg_page_eos(&og))
                        eos = 1;
                }
            }
        }
    }
    fclose(o);
    ogg_stream_clear(&os);
    vorbis_block_clear(&vb);
    vorbis_dsp_clear(&vd);
    vorbis_comment_clear(&vc);
    vorbis_info_clear(&vi);
    free(w);
    return 0;
}

static int decode(const char *in, const char *out)
{
    OggVorbis_File vf;
    if (ov_fopen(in, &vf) < 0)
        return 1;
    /* libvorbisfile 1.3.7 leaves a chained file positioned at the start of
     * its last stream after opening it. */
    if (ov_seekable(&vf) && ov_pcm_seek(&vf, 0) < 0)
        return 1;
    FILE *o = fopen(out, "wb");
    for (;;) {
        float **pcm;
        int link;
        long n = ov_read_float(&vf, &pcm, 4096, &link);
        if (n <= 0) {
            if (n < 0)
                return 1;
            break;
        }
        int channels = ov_info(&vf, link)->channels;
        for (long i = 0; i < n; i++)
            for (int k = 0; k < channels; k++) {
                int c = channels <= 8 ? maps[channels][k] : k;
                long v = lrintf(pcm[c][i] * 32768.0f);
                if (v > 32767)
                    v = 32767;
                if (v < -32768)
                    v = -32768;
                unsigned char b[2] = { (unsigned char)(v & 0xff), (unsigned char)((v >> 8) & 0xff) };
                fwrite(b, 1, 2, o);
            }
    }
    fclose(o);
    ov_clear(&vf);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 6 && !strcmp(argv[1], "encode"))
        return encode(argv[2], argv[3], (float)atof(argv[4]), atoi(argv[5]));
    if (argc == 4 && !strcmp(argv[1], "decode"))
        return decode(argv[2], argv[3]);
    fprintf(stderr, "usage: vorbisref encode IN.wav OUT.ogg QUALITY SERIAL | decode IN.ogg OUT.raw\n");
    return 2;
}
