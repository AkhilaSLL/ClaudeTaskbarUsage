/* Test aid: renders the widget bitmap off screen and composites it over a light and a dark
 * taskbar colour, to check the edges (no black box, no halo) without changing Windows settings.
 * Usage: render_preview.exe <out.bmp> <light 0|1> <five> <week> <lvl5> <lvl7> <dim 0|1> [scale [fable lvlF]]
 * Build: gcc tools\render_preview.c src\render.c src\usage.c src\json.c -lgdi32 -luser32
 * Rows of the output: light taskbar (#F3F3F3), dark taskbar (#1C1C1C). */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/render.h"

int main(int argc, char **argv)
{
    if (argc < 8) {
        printf("usage: render_preview <out.bmp> <light> <five> <week> <lvl5> <lvl7> <dim> [scale]\n");
        return 2;
    }
    render_in in;
    memset(&in, 0, sizeof(in));
    in.dpi = 96;
    in.height = 48;
    in.light = atoi(argv[2]);
    in.n = 2;
    wcscpy(in.label[0], L"5h");
    wcscpy(in.label[1], L"W");
    wcscpy(in.label[2], L"F");
    MultiByteToWideChar(CP_UTF8, 0, argv[3], -1, in.text[0], 8);
    MultiByteToWideChar(CP_UTF8, 0, argv[4], -1, in.text[1], 8);
    in.lvl[0] = atoi(argv[5]);
    in.lvl[1] = atoi(argv[6]);
    in.dim = atoi(argv[7]);
    int scale = argc > 8 ? atoi(argv[8]) : 4;
    /* Optional third value (Fable): [fable lvlF] after the scale. */
    if (argc > 10) {
        in.n = 3;
        MultiByteToWideChar(CP_UTF8, 0, argv[9], -1, in.text[2], 8);
        in.lvl[2] = atoi(argv[10]);
    }
    if (scale < 1) scale = 1;
    render_out r;
    if (!render_widget(&in, &r)) return 1;
    const DWORD *px = (const DWORD *)r.bits;
    const BYTE bgs[2][3] = { { 0xF3, 0xF3, 0xF3 }, { 0x1C, 0x1C, 0x1C } };
    int W = r.w * scale, H = r.h * 2 * scale;
    int stride = (W * 3 + 3) & ~3;
    BYTE *img = (BYTE *)calloc((size_t)stride * H, 1);
    for (int y = 0; y < H; y++) {
        int band = y / (r.h * scale);
        int sy = (y % (r.h * scale)) / scale;
        for (int x = 0; x < W; x++) {
            DWORD p = px[sy * r.w + x / scale];
            int a = (p >> 24) & 0xFF;
            int c[3] = { p & 0xFF, (p >> 8) & 0xFF, (p >> 16) & 0xFF }; /* B, G, R premultiplied */
            BYTE *o = img + (size_t)(H - 1 - y) * stride + x * 3;
            for (int k = 0; k < 3; k++) {
                int bg = bgs[band][k];
                int v = c[k] + bg * (255 - a) / 255;
                o[k] = (BYTE)(v > 255 ? 255 : v);
            }
        }
    }
    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER ih;
    memset(&fh, 0, sizeof(fh));
    memset(&ih, 0, sizeof(ih));
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + (DWORD)(stride * H);
    ih.biSize = sizeof(ih);
    ih.biWidth = W;
    ih.biHeight = H;
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    FILE *f = fopen(argv[1], "wb");
    if (!f) return 1;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    fwrite(img, 1, (size_t)stride * H, f);
    fclose(f);
    free(img);
    DeleteObject(r.bmp);
    printf("%dx%d widget, wrote %s\n", r.w, r.h, argv[1]);
    return 0;
}
