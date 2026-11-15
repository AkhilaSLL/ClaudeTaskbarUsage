/* Renders the widget into a 32-bit premultiplied ARGB DIB.
 * Text: DirectWrite through Direct2D (text_d2d.c) into a transparent layer DIB, composited over the image.
 * Fallback when Direct2D or DirectWrite cannot be created: GDI text has no alpha, so each text is drawn
 * white on black into a scratch DIB with grayscale antialiasing, and the coverage becomes the alpha of the
 * wanted colour. */
#include <windows.h>
#include <math.h>
#include <string.h>

#include <stdlib.h>

#include "logo.h"
#include "render.h"
#include "text_d2d.h"
#include "usage.h"

static int s_force_gdi, s_last_backend;

void render_force_gdi(int on) { s_force_gdi = on ? 1 : 0; }
int render_last_backend(void) { return s_last_backend; }
void render_shutdown(void) { d2d_text_shutdown(); }

/* Logo size at 96 DPI: 20 px (the spark has 12 thin rays; at 16 px they merge into a blot). */
#ifndef CTU_ICON_PX
#define CTU_ICON_PX 20
#endif

typedef struct { BYTE r, g, b; } rgb;

static HBITMAP make_dib(HDC dc, int w, int h, void **bits)
{
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h; /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    return CreateDIBSection(dc, &bi, DIB_RGB_COLORS, bits, NULL, 0);
}

/* Source-over of a colour with per-pixel coverage (0..255) times alpha (0..1) onto premultiplied ARGB. */
static void blend_px(DWORD *dst, rgb c, double a)
{
    if (a <= 0) return;
    if (a > 1) a = 1;
    DWORD d = *dst;
    double da = ((d >> 24) & 0xFF) / 255.0;
    double dr = ((d >> 16) & 0xFF) / 255.0, dg = ((d >> 8) & 0xFF) / 255.0, db = (d & 0xFF) / 255.0;
    double oa = a + da * (1 - a);
    double orr = c.r / 255.0 * a + dr * (1 - a);
    double og = c.g / 255.0 * a + dg * (1 - a);
    double ob = c.b / 255.0 * a + db * (1 - a);
    *dst = ((DWORD)(oa * 255 + 0.5) << 24) | ((DWORD)(orr * 255 + 0.5) << 16) | ((DWORD)(og * 255 + 0.5) << 8) |
           (DWORD)(ob * 255 + 0.5);
}

typedef struct {
    HDC dc;          /* memory DC with the mask DIB selected */
    DWORD *mask;     /* mask pixels */
    DWORD *dst;      /* final pixels */
    int w, h;
} canvas;

/* Draws text at (x, y) with font f into the final image with colour c and alpha a. */
static void draw_text(canvas *cv, HFONT f, const wchar_t *s, int x, int y, rgb c, double a, int light)
{
    memset(cv->mask, 0, (size_t)cv->w * cv->h * 4);
    HGDIOBJ old = SelectObject(cv->dc, f);
    SetBkMode(cv->dc, TRANSPARENT);
    SetTextColor(cv->dc, RGB(255, 255, 255));
    TextOutW(cv->dc, x, y, s, (int)wcslen(s));
    SelectObject(cv->dc, old);
    GdiFlush();
    for (int i = 0; i < cv->w * cv->h; i++) {
        DWORD m = cv->mask[i] & 0xFFFFFF;
        if (!m) continue;
        int r = (m >> 16) & 0xFF, g = (m >> 8) & 0xFF, b = m & 0xFF;
        int cov = r > g ? r : g;
        if (b > cov) cov = b;
        double cv01 = cov / 255.0;
        /* Dark text on a light background looks thin with linear coverage: lift it a little. */
        if (light) cv01 = pow(cv01, 0.8);
        blend_px(&cv->dst[i], c, cv01 * a);
    }
}

/* The Claude spark (mask from the Claude app icon, see logo.c) at (x, y), size x size, in its own orange.
 * Not dimmed with stale data. */
static void draw_logo(canvas *cv, int x, int y, int size)
{
    unsigned char *m = (unsigned char *)malloc((size_t)size * size);
    if (!m) return;
    if (ctu_logo_scale(size, m) > 0) {
        rgb c;
        ctu_logo_color(&c.r, &c.g, &c.b);
        for (int yy = 0; yy < size; yy++) {
            if (y + yy < 0 || y + yy >= cv->h) continue;
            for (int xx = 0; xx < size; xx++) {
                if (x + xx < 0 || x + xx >= cv->w) continue;
                int a = m[yy * size + xx];
                if (a) blend_px(&cv->dst[(y + yy) * cv->w + x + xx], c, a / 255.0);
            }
        }
    }
    free(m);
}

static HFONT make_font(int px, int weight, const wchar_t *face)
{
    LOGFONTW lf;
    memset(&lf, 0, sizeof(lf));
    lf.lfHeight = -px;
    lf.lfWeight = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfOutPrecision = OUT_TT_PRECIS;
    lf.lfQuality = ANTIALIASED_QUALITY;
    wcsncpy(lf.lfFaceName, face, LF_FACESIZE - 1);
    return CreateFontIndirectW(&lf);
}

/* The font only when GDI gives the face that was asked for (GetTextFaceW), else NULL. */
static HFONT make_font_exact(HDC dc, int px, int weight, const wchar_t *face)
{
    HFONT f = make_font(px, weight, face);
    if (!f) return NULL;
    HGDIOBJ old = SelectObject(dc, f);
    wchar_t got[LF_FACESIZE];
    got[0] = 0;
    GetTextFaceW(dc, LF_FACESIZE, got);
    SelectObject(dc, old);
    if (_wcsicmp(got, face) == 0) return f;
    DeleteObject(f);
    return NULL;
}

/* GDI fonts with no synthetic bold. GDI lists the weights of "Segoe UI Variable Text" as separate families;
 * a FW_SEMIBOLD request for "Segoe UI Variable Text" gets the regular face emboldened by one pixel.
 * Labels: "Segoe UI Variable Text", else "Segoe UI". Numbers: the semibold faces at FW_NORMAL; the last
 * resort is the regular "Segoe UI" (no simulated weight). */
static HFONT gdi_font(HDC dc, int px, int bold)
{
    static const wchar_t *const labels[] = { L"Segoe UI Variable Text", L"Segoe UI" };
    static const wchar_t *const nums[] = { L"Segoe UI Variable Text Semibold", L"Segoe UI Semibold", L"Segoe UI" };
    const wchar_t *const *list = bold ? nums : labels;
    int n = bold ? 3 : 2;
    for (int i = 0; i < n; i++) {
        HFONT f = make_font_exact(dc, px, FW_NORMAL, list[i]);
        if (f) return f;
    }
    return make_font(px, FW_NORMAL, L"Segoe UI");
}

int render_gdi_face(int bold, wchar_t *face, int cap, int *weight)
{
    HDC dc = CreateCompatibleDC(NULL);
    if (!dc) return 0;
    HFONT f = gdi_font(dc, 12, bold);
    HGDIOBJ old = SelectObject(dc, f);
    GetTextFaceW(dc, cap, face);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    if (weight) *weight = (int)tm.tmWeight;
    SelectObject(dc, old);
    DeleteObject(f);
    DeleteDC(dc);
    return 1;
}

/* Source-over of a premultiplied BGRA layer onto the premultiplied image. */
static void composite(DWORD *dst, const DWORD *src, int count)
{
    for (int i = 0; i < count; i++) {
        DWORD s = src[i];
        unsigned sa = s >> 24;
        if (!sa && !(s & 0xFFFFFF)) continue;
        DWORD d = dst[i];
        unsigned inv = 255 - sa, out = 0;
        for (int sh = 0; sh < 32; sh += 8) {
            unsigned sc = (s >> sh) & 0xFF, dc = (d >> sh) & 0xFF;
            unsigned v = sc + (dc * inv + 127) / 255;
            if (v > 255) v = 255;
            out |= v << sh;
        }
        dst[i] = out;
    }
}

static int text_w(HDC dc, HFONT f, const wchar_t *s)
{
    HGDIOBJ old = SelectObject(dc, f);
    SIZE sz = { 0, 0 };
    GetTextExtentPoint32W(dc, s, (int)wcslen(s), &sz);
    SelectObject(dc, old);
    return sz.cx;
}

int render_widget(const render_in *in, render_out *out)
{
    memset(out, 0, sizeof(*out));
    int dpi = in->dpi > 0 ? in->dpi : 96;
#define S(v) MulDiv((v), dpi, 96)
    HDC screen = GetDC(NULL);
    HDC dc = CreateCompatibleDC(screen);
    ReleaseDC(NULL, screen);
    if (!dc) return 0;

    /* The size of the taskbar clock text: 12 px gives digits 8 px high, the same as the clock (measured). */
    int px = S(12);
    int n = in->n == 3 ? 3 : 2;
    int wl[3], wn[3];
    ctu_metrics m;
    m.icon = S(CTU_ICON_PX);
    m.pad = S(4);
    m.gap_icon = S(6);
    m.gap_label = S(4);
    m.gap_sep = S(8);
    m.sep_w = S(1) < 1 ? 1 : S(1);

    int use_d2d = !s_force_gdi && d2d_text_init() && d2d_text_size((float)px);
    if (use_d2d) {
        m.slot = d2d_text_width(L"88%", 1);
        use_d2d = m.slot > 0;
        for (int i = 0; use_d2d && i < n; i++) {
            wl[i] = d2d_text_width(in->label[i], 0);
            wn[i] = d2d_text_width(in->text[i], 1);
            if (wl[i] < 0 || wn[i] < 0) use_d2d = 0;
        }
    }
    HFONT flabel = NULL, fnum = NULL;
    if (!use_d2d) {
        flabel = gdi_font(dc, px, 0);
        fnum = gdi_font(dc, px, 1);
        m.slot = text_w(dc, fnum, L"88%");
        for (int i = 0; i < n; i++) {
            wl[i] = text_w(dc, flabel, in->label[i]);
            wn[i] = text_w(dc, fnum, in->text[i]);
        }
    }
    s_last_backend = use_d2d ? RENDER_D2D : RENDER_GDI;
    ctu_layout lay;
    int w = ctu_bar_layout(&m, n, wl, wn, &lay);
    int icon = m.icon;
    int h = in->height > 0 ? in->height : S(48);

    void *bits = NULL, *mbits = NULL;
    HBITMAP bmp = make_dib(dc, w, h, &bits);
    HBITMAP mbmp = make_dib(dc, w, h, &mbits);
    if (!bmp || !mbmp) {
        if (bmp) DeleteObject(bmp);
        if (mbmp) DeleteObject(mbmp);
        if (flabel) DeleteObject(flabel);
        if (fnum) DeleteObject(fnum);
        DeleteDC(dc);
        return 0;
    }
    HGDIOBJ oldbmp = SelectObject(dc, mbmp);

    /* Alpha 1 everywhere: invisible, but the whole rectangle takes the mouse (alpha 0 is click-through). */
    DWORD *dst = (DWORD *)bits;
    for (int i = 0; i < w * h; i++) dst[i] = 0x01000000;

    canvas cv = { dc, (DWORD *)mbits, dst, w, h };

    rgb text = in->light ? (rgb){ 0x1A, 0x1A, 0x1A } : (rgb){ 0xFF, 0xFF, 0xFF };
    rgb orange = in->light ? (rgb){ 0xB8, 0x55, 0x00 } : (rgb){ 0xFF, 0xA5, 0x4A };
    rgb red = in->light ? (rgb){ 0xC4, 0x2B, 0x1C } : (rgb){ 0xFF, 0x6B, 0x6B };
    /* Labels: a secondary colour, but not washed out (was 0.66 / 0.62). Not dimmed with stale data. */
    double label_a = in->light ? CTU_LABEL_ALPHA_LIGHT : CTU_LABEL_ALPHA_DARK;
    double num_a = in->dim ? 0.55 : 1.0;

    /* Centre the ink of digits and capitals (cap height above the baseline) in the window:
     * baseline = centre + half the cap height, on a whole pixel row. */
    int cy = h / 2;
    int cap, ascent = 0;
    if (use_d2d) {
        cap = (int)lroundf(d2d_text_cap(1));
        if (cap <= 0) cap = px * 7 / 10;
    } else {
        TEXTMETRICW tm;
        HGDIOBJ of = SelectObject(dc, fnum);
        GetTextMetricsW(dc, &tm);
        cap = (tm.tmAscent - tm.tmInternalLeading) * 7 / 10;
        OUTLINETEXTMETRICW otm;
        otm.otmSize = sizeof(otm);
        if (GetOutlineTextMetricsW(dc, sizeof(otm), &otm) && otm.otmsCapEmHeight > 0) cap = (int)otm.otmsCapEmHeight;
        SelectObject(dc, of);
        ascent = tm.tmAscent;
    }
    int baseline = cy + (cap + 1) / 2;

    draw_logo(&cv, lay.icon_x, cy - icon / 2, icon);

    int sh = S(14);
    for (int i = 1; i < n; i++) {
        int sx = lay.sep_x[i - 1];
        for (int yy = cy - sh / 2; yy < cy - sh / 2 + sh; yy++)
            for (int xx = sx; xx < sx + m.sep_w; xx++)
                if (yy >= 0 && yy < h && xx >= 0 && xx < w) blend_px(&dst[yy * w + xx], text, 0.30);
    }

    int drawn = 0;
    if (use_d2d) {
        d2d_text_item items[6];
        int k = 0;
        for (int i = 0; i < n; i++) {
            rgb nc = in->lvl[i] == 2 ? red : in->lvl[i] == 1 ? orange : text;
            items[k++] = (d2d_text_item){ in->label[i], 0, lay.label_x[i], baseline,
                                          text.r / 255.0f, text.g / 255.0f, text.b / 255.0f, (float)label_a };
            items[k++] = (d2d_text_item){ in->text[i], 1, lay.num_x[i], baseline,
                                          nc.r / 255.0f, nc.g / 255.0f, nc.b / 255.0f, (float)num_a };
        }
        if (d2d_text_draw(dc, w, h, items, k)) {
            GdiFlush();
            composite(dst, (const DWORD *)mbits, w * h);
            drawn = 1;
        }
    }
    if (!drawn) {
        if (use_d2d) {
            /* The D2D draw failed after the layout: draw the texts with GDI in the same layout (the widths
             * differ by a pixel at most). */
            flabel = gdi_font(dc, px, 0);
            fnum = gdi_font(dc, px, 1);
            TEXTMETRICW tm;
            HGDIOBJ of = SelectObject(dc, fnum);
            GetTextMetricsW(dc, &tm);
            SelectObject(dc, of);
            ascent = tm.tmAscent;
            s_last_backend = RENDER_GDI;
        }
        int ty = baseline - ascent;
        for (int i = 0; i < n; i++) {
            draw_text(&cv, flabel, in->label[i], lay.label_x[i], ty, text, label_a, in->light);
            rgb nc = in->lvl[i] == 2 ? red : in->lvl[i] == 1 ? orange : text;
            draw_text(&cv, fnum, in->text[i], lay.num_x[i], ty, nc, num_a, in->light);
        }
    }

    SelectObject(dc, oldbmp);
    DeleteObject(mbmp);
    if (flabel) DeleteObject(flabel);
    if (fnum) DeleteObject(fnum);
    DeleteDC(dc);
#undef S
    out->bmp = bmp;
    out->bits = bits;
    out->w = w;
    out->h = h;
    return 1;
}
