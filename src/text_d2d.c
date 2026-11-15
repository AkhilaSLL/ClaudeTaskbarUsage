/* Widget text with DirectWrite through Direct2D (see text_d2d.h). C COM interfaces of the mingw-w64 headers;
 * no method that returns a structure is called (those are not usable from C). */
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <d2d1.h>
#include <dwrite.h>
#include <dwrite_1.h>
#include <math.h>
#include <string.h>
#include <wchar.h>

#include "text_d2d.h"

/* Every interface starts with the IUnknown methods (the d2d1.h C vtables nest them in a base member). */
#define RELEASE(p)                                              \
    do {                                                        \
        if (p) {                                                \
            ((IUnknown *)(p))->lpVtbl->Release((IUnknown *)(p)); \
            (p) = NULL;                                         \
        }                                                       \
    } while (0)

static ID2D1Factory *s_d2d;
static IDWriteFactory *s_dw;
static ID2D1DCRenderTarget *s_rt;
static IDWriteRenderingParams *s_params;
static IDWriteTextFormat *s_fmt[2];
static float s_px;
static wchar_t s_family[64];
static int s_failed;

static DWRITE_FONT_WEIGHT weight_of(int bold) { return bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL; }

static int has_family(const wchar_t *name)
{
    IDWriteFontCollection *col = NULL;
    UINT32 index = 0;
    BOOL exists = FALSE;
    if (SUCCEEDED(IDWriteFactory_GetSystemFontCollection(s_dw, &col, FALSE)) && col)
        IDWriteFontCollection_FindFamilyName(col, name, &index, &exists);
    RELEASE(col);
    return exists ? 1 : 0;
}

static int make_target(void)
{
    if (s_rt) return 1;
    D2D1_RENDER_TARGET_PROPERTIES rp;
    memset(&rp, 0, sizeof(rp));
    rp.type = D2D1_RENDER_TARGET_TYPE_SOFTWARE; /* no GPU device: small, and the same pixels everywhere */
    rp.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    rp.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    rp.dpiX = 96.0f; /* sizes are given in pixels */
    rp.dpiY = 96.0f;
    rp.usage = D2D1_RENDER_TARGET_USAGE_NONE;
    rp.minLevel = D2D1_FEATURE_LEVEL_DEFAULT;
    return SUCCEEDED(ID2D1Factory_CreateDCRenderTarget(s_d2d, &rp, &s_rt)) && s_rt;
}

/* Rendering parameters like XAML: the system gamma and contrast values, grayscale (ClearType level 0, flat
 * pixel geometry), natural symmetric rendering mode. */
static void make_params(void)
{
    float gamma = 1.8f, contrast = 0.5f, gray = 0.5f;
    IDWriteRenderingParams *def = NULL;
    if (SUCCEEDED(IDWriteFactory_CreateRenderingParams(s_dw, &def)) && def) {
        gamma = IDWriteRenderingParams_GetGamma(def);
        contrast = IDWriteRenderingParams_GetEnhancedContrast(def);
        gray = contrast;
        IDWriteRenderingParams1 *p1 = NULL;
        if (SUCCEEDED(def->lpVtbl->QueryInterface(def, &IID_IDWriteRenderingParams1, (void **)&p1)) && p1)
            gray = IDWriteRenderingParams1_GetGrayscaleEnhancedContrast(p1);
        RELEASE(p1);
    }
    RELEASE(def);
    IDWriteFactory1 *f1 = NULL;
    if (SUCCEEDED(s_dw->lpVtbl->QueryInterface(s_dw, &IID_IDWriteFactory1, (void **)&f1)) && f1) {
        IDWriteRenderingParams1 *p = NULL;
        if (SUCCEEDED(IDWriteFactory1_CreateCustomRenderingParams(f1, gamma, contrast, gray, 0.0f, DWRITE_PIXEL_GEOMETRY_FLAT,
                                                                  DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC, &p)) && p)
            s_params = (IDWriteRenderingParams *)p;
        RELEASE(f1);
    }
    if (!s_params)
        IDWriteFactory_CreateCustomRenderingParams(s_dw, gamma, gray, 0.0f, DWRITE_PIXEL_GEOMETRY_FLAT,
                                                   DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC, &s_params);
}

/* d2d1.dll and dwrite.dll are loaded at run time, so that the exe also starts without them (GDI fallback). */
typedef HRESULT(WINAPI *d2d_create_fn)(D2D1_FACTORY_TYPE, REFIID, const D2D1_FACTORY_OPTIONS *, void **);
typedef HRESULT(WINAPI *dw_create_fn)(DWRITE_FACTORY_TYPE, REFIID, IUnknown **);

int d2d_text_init(void)
{
    if (s_rt && s_params) return 1;
    if (s_failed) return 0;
    if (!s_d2d) {
        HMODULE m = LoadLibraryExW(L"d2d1.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        d2d_create_fn f = m ? (d2d_create_fn)(void *)GetProcAddress(m, "D2D1CreateFactory") : NULL;
        if (!f || FAILED(f(D2D1_FACTORY_TYPE_SINGLE_THREADED, &IID_ID2D1Factory, NULL, (void **)&s_d2d))) s_d2d = NULL;
    }
    if (s_d2d && !s_dw) {
        HMODULE m = LoadLibraryExW(L"dwrite.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        dw_create_fn f = m ? (dw_create_fn)(void *)GetProcAddress(m, "DWriteCreateFactory") : NULL;
        if (!f || FAILED(f(DWRITE_FACTORY_TYPE_SHARED, &IID_IDWriteFactory, (IUnknown **)&s_dw))) s_dw = NULL;
    }
    if (!s_d2d || !s_dw) {
        s_failed = 1;
        d2d_text_shutdown();
        s_failed = 1;
        return 0;
    }
    if (!s_family[0]) wcscpy(s_family, has_family(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI");
    if (!s_params) make_params();
    if (!s_params || !make_target()) {
        s_failed = 1;
        d2d_text_shutdown();
        s_failed = 1;
        return 0;
    }
    return 1;
}

void d2d_text_shutdown(void)
{
    RELEASE(s_fmt[0]);
    RELEASE(s_fmt[1]);
    RELEASE(s_rt);
    RELEASE(s_params);
    RELEASE(s_dw);
    RELEASE(s_d2d);
    s_px = 0;
    s_family[0] = 0;
    s_failed = 0;
}

int d2d_text_size(float px)
{
    if (!s_dw) return 0;
    if (px == s_px && s_fmt[0] && s_fmt[1]) return 1;
    RELEASE(s_fmt[0]);
    RELEASE(s_fmt[1]);
    s_px = 0;
    for (int b = 0; b < 2; b++) {
        if (FAILED(IDWriteFactory_CreateTextFormat(s_dw, s_family, NULL, weight_of(b), DWRITE_FONT_STYLE_NORMAL,
                                                   DWRITE_FONT_STRETCH_NORMAL, px, L"en-us", &s_fmt[b])) ||
            !s_fmt[b]) {
            RELEASE(s_fmt[0]);
            RELEASE(s_fmt[1]);
            return 0;
        }
        IDWriteTextFormat_SetWordWrapping(s_fmt[b], DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    s_px = px;
    return 1;
}

static IDWriteTextLayout *layout_of(const wchar_t *s, int bold)
{
    IDWriteTextLayout *lay = NULL;
    if (!s_fmt[bold ? 1 : 0]) return NULL;
    if (FAILED(IDWriteFactory_CreateTextLayout(s_dw, s, (UINT32)wcslen(s), s_fmt[bold ? 1 : 0], 4096.0f, 1024.0f, &lay)))
        return NULL;
    return lay;
}

int d2d_text_width(const wchar_t *s, int bold)
{
    IDWriteTextLayout *lay = layout_of(s, bold);
    if (!lay) return -1;
    DWRITE_TEXT_METRICS tm;
    int w = -1;
    if (SUCCEEDED(IDWriteTextLayout_GetMetrics(lay, &tm))) w = (int)ceilf(tm.widthIncludingTrailingWhitespace - 0.01f);
    RELEASE(lay);
    return w < 0 ? -1 : w;
}

static IDWriteFont *font_of(int bold)
{
    IDWriteFontCollection *col = NULL;
    IDWriteFontFamily *fam = NULL;
    IDWriteFont *font = NULL;
    UINT32 index = 0;
    BOOL exists = FALSE;
    if (s_dw && SUCCEEDED(IDWriteFactory_GetSystemFontCollection(s_dw, &col, FALSE)) && col &&
        SUCCEEDED(IDWriteFontCollection_FindFamilyName(col, s_family, &index, &exists)) && exists &&
        SUCCEEDED(IDWriteFontCollection_GetFontFamily(col, index, &fam)) && fam)
        IDWriteFontFamily_GetFirstMatchingFont(fam, weight_of(bold), DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                               &font);
    RELEASE(fam);
    RELEASE(col);
    return font;
}

float d2d_text_cap(int bold)
{
    IDWriteFont *font = font_of(bold);
    if (!font) return 0;
    DWRITE_FONT_METRICS fm;
    IDWriteFont_GetMetrics(font, &fm);
    RELEASE(font);
    return fm.designUnitsPerEm ? (float)fm.capHeight * s_px / (float)fm.designUnitsPerEm : 0;
}

int d2d_text_face(int bold, wchar_t *family, int cap, int *weight, int *simulations)
{
    IDWriteFont *font = font_of(bold);
    if (!font) return 0;
    if (family && cap > 0) {
        wcsncpy(family, s_family, (size_t)cap - 1);
        family[cap - 1] = 0;
    }
    if (weight) *weight = (int)IDWriteFont_GetWeight(font);
    if (simulations) *simulations = (int)IDWriteFont_GetSimulations(font);
    RELEASE(font);
    return 1;
}

int d2d_text_draw(HDC dc, int w, int h, const d2d_text_item *items, int n)
{
    if (!s_rt || !s_params || !s_fmt[0] || !s_fmt[1]) return 0;
    RECT rc = { 0, 0, w, h };
    if (FAILED(ID2D1DCRenderTarget_BindDC(s_rt, dc, &rc))) return 0;
    ID2D1RenderTarget *rt = (ID2D1RenderTarget *)s_rt;
    ID2D1RenderTarget_SetTextAntialiasMode(rt, D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    ID2D1RenderTarget_SetTextRenderingParams(rt, s_params);
    ID2D1RenderTarget_BeginDraw(rt);
    D2D1_COLOR_F clear = { 0, 0, 0, 0 };
    ID2D1RenderTarget_Clear(rt, &clear);
    ID2D1SolidColorBrush *brush = NULL;
    int ok = SUCCEEDED(ID2D1RenderTarget_CreateSolidColorBrush(rt, &clear, NULL, &brush)) && brush;
    for (int i = 0; ok && i < n; i++) {
        IDWriteTextLayout *lay = layout_of(items[i].s, items[i].bold);
        if (!lay) {
            ok = 0;
            break;
        }
        DWRITE_LINE_METRICS lm;
        UINT32 count = 0;
        if (FAILED(IDWriteTextLayout_GetLineMetrics(lay, &lm, 1, &count)) || count < 1) {
            RELEASE(lay);
            ok = 0;
            break;
        }
        D2D1_COLOR_F c = { items[i].r, items[i].g, items[i].b, items[i].a };
        ID2D1SolidColorBrush_SetColor(brush, &c);
        /* The layout's top-left corner such that the baseline lands on the whole pixel row. */
        D2D1_POINT_2F o = { (float)items[i].x, (float)items[i].baseline - lm.baseline };
        ID2D1RenderTarget_DrawTextLayout(rt, o, lay, (ID2D1Brush *)brush, D2D1_DRAW_TEXT_OPTIONS_NONE);
        RELEASE(lay);
    }
    RELEASE(brush);
    HRESULT hr = ID2D1RenderTarget_EndDraw(rt, NULL, NULL);
    if (hr == (HRESULT)D2DERR_RECREATE_TARGET) RELEASE(s_rt); /* made again by the next d2d_text_init */
    return ok && SUCCEEDED(hr);
}
