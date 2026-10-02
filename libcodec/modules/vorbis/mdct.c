/* The inverse MDCT of Vorbis,
 *
 *   y[n] = sum over k < N/2 of X[k] cos(2 pi / N (n + 1/2 + N/4) (k + 1/2)),
 *
 * for n < N, computed through a DCT-IV of length M = N/2. The DCT-IV is a
 * complex FFT of length M/2 between a rotation of the input pairs
 * (X[2k], X[M-1-2k]) by exp(-i pi (4k + 1) / (4M)) and a rotation of the
 * result by exp(-i pi k / M). The IMDCT then reads the DCT-IV u forwards
 * and backwards with changed signs:
 *
 *   y[n] = u[n + M/2]            for n < M/2,
 *   y[n] = -u[3M/2 - 1 - n]      for M/2 <= n < 3M/2,
 *   y[n] = -u[n - 3M/2]          for 3M/2 <= n < 2M. */
#include "vorbis.h"
#include <math.h>

int vb_mdct_init(struct vb_mdct *m, unsigned n)
{
    memset(m, 0, sizeof *m);
    unsigned half = n / 2, quarter = n / 4;
    m->n = n;
    m->twiddle = malloc(sizeof *m->twiddle * 2 * quarter);
    m->post = malloc(sizeof *m->post * 2 * quarter);
    m->fft_cos = malloc(sizeof *m->fft_cos * quarter);
    m->fft_sin = malloc(sizeof *m->fft_sin * quarter);
    m->bitrev = malloc(sizeof *m->bitrev * quarter);
    m->work = malloc(sizeof *m->work * (2 * quarter + half));
    if (!m->twiddle || !m->post || !m->fft_cos || !m->fft_sin || !m->bitrev || !m->work) {
        vb_mdct_free(m);
        return -ENOMEM;
    }
    for (unsigned k = 0; k < quarter; k++) {
        double a = -M_PI * (4.0 * k + 1) / (4.0 * half), b = -M_PI * k / half;
        m->twiddle[2 * k] = (float)cos(a);
        m->twiddle[2 * k + 1] = (float)sin(a);
        m->post[2 * k] = (float)cos(b);
        m->post[2 * k + 1] = (float)sin(b);
        m->fft_cos[k] = (float)cos(-2 * M_PI * k / quarter);
        m->fft_sin[k] = (float)sin(-2 * M_PI * k / quarter);
    }
    unsigned bits = 0;
    while ((1u << bits) < quarter)
        bits++;
    for (unsigned k = 0; k < quarter; k++) {
        unsigned r = 0;
        for (unsigned b = 0; b < bits; b++)
            r |= ((k >> b) & 1) << (bits - 1 - b);
        m->bitrev[k] = r;
    }
    return 0;
}

void vb_mdct_free(struct vb_mdct *m)
{
    free(m->twiddle);
    free(m->post);
    free(m->fft_cos);
    free(m->fft_sin);
    free(m->bitrev);
    free(m->work);
    memset(m, 0, sizeof *m);
}

/* An in-place radix-2 FFT of q complex values stored as re, im pairs. */
static void fft(const struct vb_mdct *m, float *z, unsigned q)
{
    for (unsigned k = 0; k < q; k++) {
        unsigned r = m->bitrev[k];
        if (r > k) {
            float t0 = z[2 * k], t1 = z[2 * k + 1];
            z[2 * k] = z[2 * r];
            z[2 * k + 1] = z[2 * r + 1];
            z[2 * r] = t0;
            z[2 * r + 1] = t1;
        }
    }
    for (unsigned len = 2; len <= q; len *= 2) {
        unsigned step = q / len;
        for (unsigned start = 0; start < q; start += len)
            for (unsigned j = 0; j < len / 2; j++) {
                float wr = m->fft_cos[j * step], wi = m->fft_sin[j * step];
                float *a = z + 2 * (start + j), *b = z + 2 * (start + j + len / 2);
                float tr = b[0] * wr - b[1] * wi, ti = b[0] * wi + b[1] * wr;
                b[0] = a[0] - tr;
                b[1] = a[1] - ti;
                a[0] += tr;
                a[1] += ti;
            }
    }
}

void vb_imdct(const struct vb_mdct *m, const float *in, float *out)
{
    unsigned half = m->n / 2, quarter = m->n / 4;
    float *z = m->work, *u = m->work + 2 * quarter;
    for (unsigned k = 0; k < quarter; k++) {
        float re = in[2 * k], im = in[half - 1 - 2 * k];
        float c = m->twiddle[2 * k], s = m->twiddle[2 * k + 1];
        z[2 * k] = re * c - im * s;
        z[2 * k + 1] = re * s + im * c;
    }
    fft(m, z, quarter);
    for (unsigned k = 0; k < quarter; k++) {
        float re = z[2 * k], im = z[2 * k + 1];
        float c = m->post[2 * k], s = m->post[2 * k + 1];
        u[2 * k] = re * c - im * s;
        u[half - 1 - 2 * k] = -(re * s + im * c);
    }
    unsigned h2 = half / 2;
    for (unsigned n = 0; n < h2; n++)
        out[n] = u[n + h2];
    for (unsigned n = h2; n < 3 * h2; n++)
        out[n] = -u[3 * h2 - 1 - n];
    for (unsigned n = 3 * h2; n < m->n; n++)
        out[n] = -u[n - 3 * h2];
}

void vb_imdct_direct(unsigned n, const float *in, float *out)
{
    unsigned half = n / 2;
    for (unsigned i = 0; i < n; i++) {
        double sum = 0;
        for (unsigned k = 0; k < half; k++)
            sum += in[k] * cos(2 * M_PI / n * (i + 0.5 + n / 4.0) * (k + 0.5));
        out[i] = (float)sum;
    }
}
