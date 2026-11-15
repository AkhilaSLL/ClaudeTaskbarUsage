/* The Claude spark mask and its area-average scaler. */
#include <stdlib.h>
#include <string.h>

#include "logo.h"
#include "logo_mask.h"

int ctu_logo_n(void) { return CTU_LOGO_N; }

const unsigned char *ctu_logo_mask(void) { return ctu_logo_data; }

void ctu_logo_color(unsigned char *r, unsigned char *g, unsigned char *b)
{
    *r = CTU_LOGO_R;
    *g = CTU_LOGO_G;
    *b = CTU_LOGO_B;
}

/* Overlap of source pixel s ([s, s+1)) with output pixel o ([o*N/size, (o+1)*N/size)), in source pixels. */
static double overlap(int s, int o, int size)
{
    double a = (double)o * CTU_LOGO_N / size, b = (double)(o + 1) * CTU_LOGO_N / size;
    double lo = s > a ? s : a, hi = s + 1 < b ? s + 1 : b;
    return hi > lo ? hi - lo : 0.0;
}

int ctu_logo_scale(int size, unsigned char *out)
{
    if (size < 1 || size > 256 || !out) return -1;
    const int n = CTU_LOGO_N;
    double *tmp = (double *)calloc((size_t)n * size, sizeof(double)); /* n rows x size columns */
    if (!tmp) return -1;
    const double span = (double)n / size; /* source pixels per output pixel along one axis */
    /* Horizontal pass. */
    for (int ox = 0; ox < size; ox++) {
        int s0 = (int)((double)ox * n / size), s1 = (int)((double)(ox + 1) * n / size + 1.0);
        if (s1 > n) s1 = n;
        for (int s = s0; s < s1; s++) {
            double wgt = overlap(s, ox, size);
            if (wgt <= 0) continue;
            for (int y = 0; y < n; y++) tmp[(size_t)y * size + ox] += wgt * ctu_logo_data[(size_t)y * n + s];
        }
    }
    /* Vertical pass. */
    int nonzero = 0;
    for (int oy = 0; oy < size; oy++) {
        int s0 = (int)((double)oy * n / size), s1 = (int)((double)(oy + 1) * n / size + 1.0);
        if (s1 > n) s1 = n;
        for (int ox = 0; ox < size; ox++) {
            double acc = 0;
            for (int s = s0; s < s1; s++) {
                double wgt = overlap(s, oy, size);
                if (wgt > 0) acc += wgt * tmp[(size_t)s * size + ox];
            }
            int v = (int)(acc / (span * span) + 0.5);
            if (v > 255) v = 255;
            if (v < 0) v = 0;
            out[oy * size + ox] = (unsigned char)v;
            if (v) nonzero++;
        }
    }
    free(tmp);
    return nonzero;
}
