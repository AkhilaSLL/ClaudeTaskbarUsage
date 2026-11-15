/* Widget text with DirectWrite through Direct2D (grayscale antialiasing, natural symmetric rendering, the
 * rasterizer of the taskbar). The factories and the DC render target are created once and kept. */
#ifndef CTU_TEXT_D2D_H
#define CTU_TEXT_D2D_H

#include <windows.h>

typedef struct {
    const wchar_t *s;
    int bold;           /* 1 = numbers (semi-bold), 0 = labels (regular) */
    int x;              /* left edge, whole pixels */
    int baseline;       /* baseline, whole pixels */
    float r, g, b, a;   /* straight colour 0..1 and alpha 0..1 */
} d2d_text_item;

/* Creates the factories and the render target once. Returns 0 when Direct2D or DirectWrite is missing. */
int d2d_text_init(void);
/* Releases everything (at exit). */
void d2d_text_shutdown(void);
/* Font size in pixels (96 DPI DIPs = pixels) for the next calls; creates the two text formats. */
int d2d_text_size(float px);
/* Advance width in whole pixels (rounded up), -1 on failure. */
int d2d_text_width(const wchar_t *s, int bold);
/* Cap height in pixels of the current size. */
float d2d_text_cap(int bold);
/* The face DirectWrite chose: family name, weight and simulations (0 = none, no synthetic bold). */
int d2d_text_face(int bold, wchar_t *family, int cap, int *weight, int *simulations);
/* Draws the items into the 32-bit DIB selected in dc (w x h), which is cleared to transparent first.
 * The result is premultiplied BGRA. Returns 1 on success. */
int d2d_text_draw(HDC dc, int w, int h, const d2d_text_item *items, int n);

#endif
