/* Renders the widget into a 32-bit premultiplied ARGB DIB (for UpdateLayeredWindow). */
#ifndef CTU_RENDER_H
#define CTU_RENDER_H

#include <windows.h>

typedef struct {
    int dpi;            /* monitor DPI of the taskbar */
    int light;          /* 1 = light taskbar: near-black text */
    int height;         /* window height in pixels (the taskbar height) */
    int n;              /* number of values: 2 (5h, W) or 3 (5h, W, F) */
    wchar_t label[3][4];/* L"5h", L"W", L"F" */
    wchar_t text[3][8]; /* value texts, for example L"24%" or L"--" */
    int lvl[3];         /* 0 normal, 1 orange, 2 red */
    int dim;            /* stale data: numbers at about 55% alpha */
} render_in;

typedef struct {
    HBITMAP bmp;        /* DIB section, top-down, 32 bpp premultiplied */
    void *bits;
    int w, h;
} render_out;

/* Returns 1 on success. Free out->bmp with DeleteObject. */
int render_widget(const render_in *in, render_out *out);

/* Label alpha on a dark and on a light taskbar. */
#define CTU_LABEL_ALPHA_DARK 0.80
#define CTU_LABEL_ALPHA_LIGHT 0.72

enum { RENDER_NONE = 0, RENDER_D2D = 1, RENDER_GDI = 2 };
/* 1 = always use the GDI text fallback (test aid CTU_TEXT=gdi and the tests). */
void render_force_gdi(int on);
/* The text method of the last render_widget call (RENDER_D2D or RENDER_GDI). */
int render_last_backend(void);
/* The GDI fallback face for labels (bold 0) or numbers (bold 1), with its TEXTMETRIC weight. */
int render_gdi_face(int bold, wchar_t *face, int cap, int *weight);
/* Releases the Direct2D and DirectWrite objects (at exit). */
void render_shutdown(void);

#endif
