/* The window functions of Vorbis (section 4.3.1 of the specification). */
#include "vorbis.h"
#include <math.h>

void vb_window_ramp(float *ramp, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        double x = sin((i + 0.5) / n * M_PI / 2);
        ramp[i] = (float)sin(M_PI / 2 * x * x);
    }
}

void vb_window(float *const ramp[2], const unsigned blocksize[2], float *v, unsigned n, unsigned blockflag,
               unsigned prevflag, unsigned nextflag)
{
    unsigned bs0 = blocksize[0];
    unsigned ls, le, rs, re;
    const float *lramp, *rramp;
    if (blockflag && !prevflag) {
        ls = n / 4 - bs0 / 4;
        le = n / 4 + bs0 / 4;
        lramp = ramp[0];
    } else {
        ls = 0;
        le = n / 2;
        lramp = ramp[blockflag];
    }
    if (blockflag && !nextflag) {
        rs = n * 3 / 4 - bs0 / 4;
        re = n * 3 / 4 + bs0 / 4;
        rramp = ramp[0];
    } else {
        rs = n / 2;
        re = n;
        rramp = ramp[blockflag];
    }
    for (unsigned i = 0; i < ls; i++)
        v[i] = 0;
    for (unsigned i = ls; i < le; i++)
        v[i] *= lramp[i - ls];
    for (unsigned i = rs; i < re; i++)
        v[i] *= rramp[re - 1 - i];
    for (unsigned i = re; i < n; i++)
        v[i] = 0;
}
