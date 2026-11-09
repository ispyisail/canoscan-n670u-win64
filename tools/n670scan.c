/*
 * n670scan - command-line scanner for the Canon CanoScan N670U/N676U/LiDE20
 * on Windows x64, using the SANE plustek backend over usbscan.sys.
 *
 *   n670scan --list
 *   n670scan [--mode color|gray|lineart] [--res DPI] [--area X Y W H (mm)]
 *            [--debug N] [--log FILE] -o out.bmp|out.pnm
 */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "n670_scan.h"
#include "n670_win32.h"

static void usage(void)
{
    fprintf(stderr,
        "usage: n670scan --list\n"
        "       n670scan [options] -o FILE(.bmp|.pnm)\n"
        "options:\n"
        "  --mode color|gray|lineart   (default color)\n"
        "  --res DPI                   75..600 (default 300)\n"
        "  --area X Y W H              scan window in mm (default full bed)\n"
        "  --preview                   fast preview pass\n"
        "  --gamma G                   tone curve (default 1.0 = linear; <1 darker)\n"
        "  --white PCT                 white level 50..120 (100 = SANE default;\n"
        "                              lower keeps highlight detail)\n"
        "  --debug N                   SANE debug level (e.g. 10, 255)\n"
        "  --log FILE                  write debug log to FILE\n");
}

static int ends_with(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && _stricmp(s + a - b, suf) == 0;
}

static int write_pnm(const char *path, const N670Frame *f,
                     const unsigned char *img, int lines)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;
    if (f->depth == 1)
        fprintf(fp, "P4\n%d %d\n", f->pixels_per_line, lines);
    else
        fprintf(fp, "P%d\n%d %d\n255\n", f->channels == 3 ? 6 : 5,
                f->pixels_per_line, lines);
    fwrite(img, (size_t)f->bytes_per_line, (size_t)lines, fp);
    fclose(fp);
    return 0;
}

static int write_bmp(const char *path, const N670Frame *f,
                     const unsigned char *img, int lines)
{
    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER ih;
    RGBQUAD pal[256];
    int bpp = f->depth == 1 ? 1 : (f->channels == 3 ? 24 : 8);
    int ncolors = bpp == 24 ? 0 : (bpp == 1 ? 2 : 256);
    int stride = ((f->pixels_per_line * bpp + 31) / 32) * 4;
    unsigned char *row;
    FILE *fp;
    int y, x, i;

    fp = fopen(path, "wb");
    if (!fp) return -1;
    row = (unsigned char *)calloc(1, (size_t)stride);
    memset(&fh, 0, sizeof(fh));
    memset(&ih, 0, sizeof(ih));
    fh.bfType    = 0x4d42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih) + ncolors * sizeof(RGBQUAD);
    fh.bfSize    = fh.bfOffBits + (DWORD)stride * lines;
    ih.biSize    = sizeof(ih);
    ih.biWidth   = f->pixels_per_line;
    ih.biHeight  = lines;             /* bottom-up */
    ih.biPlanes  = 1;
    ih.biBitCount = (WORD)bpp;
    ih.biSizeImage = (DWORD)stride * lines;
    ih.biClrUsed = ncolors;
    fwrite(&fh, sizeof(fh), 1, fp);
    fwrite(&ih, sizeof(ih), 1, fp);
    if (bpp == 8) {
        for (i = 0; i < 256; i++) {
            pal[i].rgbRed = pal[i].rgbGreen = pal[i].rgbBlue = (BYTE)i;
            pal[i].rgbReserved = 0;
        }
        fwrite(pal, sizeof(RGBQUAD), 256, fp);
    } else if (bpp == 1) {
        /* SANE lineart: bit 1 = black */
        pal[0].rgbRed = pal[0].rgbGreen = pal[0].rgbBlue = 255; pal[0].rgbReserved = 0;
        pal[1].rgbRed = pal[1].rgbGreen = pal[1].rgbBlue = 0;   pal[1].rgbReserved = 0;
        fwrite(pal, sizeof(RGBQUAD), 2, fp);
    }
    for (y = lines - 1; y >= 0; y--) {
        const unsigned char *src = img + (size_t)y * f->bytes_per_line;
        memset(row, 0, (size_t)stride);
        if (bpp == 24) {
            for (x = 0; x < f->pixels_per_line; x++) {
                row[x * 3 + 0] = src[x * 3 + 2];
                row[x * 3 + 1] = src[x * 3 + 1];
                row[x * 3 + 2] = src[x * 3 + 0];
            }
        } else {
            memcpy(row, src, (size_t)f->bytes_per_line);
        }
        fwrite(row, (size_t)stride, 1, fp);
    }
    free(row);
    fclose(fp);
    return 0;
}

int main(int argc, char **argv)
{
    N670Params p;
    N670Frame f;
    N670Bed bed;
    N670Session *s = NULL;
    const char *out = NULL, *logfile = NULL;
    int debug = -1, list = 0, i, lines;
    unsigned char *img;
    size_t cap, used = 0;
    long n;
    DWORD t0;

    memset(&p, 0, sizeof(p));
    p.mode = N670_MODE_COLOR;
    p.dpi  = 300;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--list")) list = 1;
        else if (!strcmp(argv[i], "--preview")) p.preview = 1;
        else if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
            const char *m = argv[++i];
            if (!_stricmp(m, "gray") || !_stricmp(m, "grey")) p.mode = N670_MODE_GRAY;
            else if (!_stricmp(m, "lineart") || !_stricmp(m, "bw")) p.mode = N670_MODE_LINEART;
            else p.mode = N670_MODE_COLOR;
        }
        else if (!strcmp(argv[i], "--res") && i + 1 < argc) p.dpi = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--area") && i + 4 < argc) {
            p.x_mm = atof(argv[++i]); p.y_mm = atof(argv[++i]);
            p.w_mm = atof(argv[++i]); p.h_mm = atof(argv[++i]);
        }
        else if (!strcmp(argv[i], "--gamma") && i + 1 < argc) n670_set_gamma(atof(argv[++i]));
        else if (!strcmp(argv[i], "--debug") && i + 1 < argc) debug = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--log") && i + 1 < argc) logfile = argv[++i];
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else { usage(); return 2; }
    }
    if (!list && !out) { usage(); return 2; }

    n670_set_log(logfile, debug);

    if (n670_init() != 0) {
        fprintf(stderr, "init failed: %s\n", n670_last_error());
        return 1;
    }
    if (list) {
        char buf[4096];
        int cnt = n670_list(buf, sizeof(buf));
        printf("%d device(s)\n%s", cnt, buf);
        n670_exit();
        return cnt > 0 ? 0 : 1;
    }

    if (n670_open(NULL, &s, &bed) != 0) {
        fprintf(stderr, "open failed: %s\n", n670_last_error());
        n670_exit();
        return 1;
    }
    printf("bed %.1f x %.1f mm, max %d dpi\n", bed.w_mm, bed.h_mm, bed.max_dpi);

    t0 = GetTickCount();
    if (n670_start(s, &p, &f) != 0) {
        fprintf(stderr, "start failed: %s\n", n670_last_error());
        n670_close(s);
        n670_exit();
        return 1;
    }
    printf("scanning %d x %d px (%d bytes/line)...\n",
           f.pixels_per_line, f.lines, f.bytes_per_line);

    cap = (size_t)f.bytes_per_line * (f.lines > 0 ? f.lines : 1000);
    img = (unsigned char *)malloc(cap);
    for (;;) {
        if (used == cap) {
            cap *= 2;
            img = (unsigned char *)realloc(img, cap);
        }
        n = n670_read(s, img + used, (long)(cap - used));
        if (n < 0) {
            fprintf(stderr, "\nread failed: %s\n", n670_last_error());
            break;
        }
        if (n == 0) break;
        used += (size_t)n;
        if (f.lines > 0)
            printf("\r%3d%%", (int)(100.0 * used / ((double)f.bytes_per_line * f.lines)));
    }
    n670_close(s);
    n670_exit();

    lines = (int)(used / (size_t)f.bytes_per_line);
    printf("\r%d lines in %.1fs\n", lines, (GetTickCount() - t0) / 1000.0);
    if (lines <= 0) {
        free(img);
        return 1;
    }
    if ((ends_with(out, ".bmp") ? write_bmp : write_pnm)(out, &f, img, lines) != 0) {
        fprintf(stderr, "cannot write %s\n", out);
        free(img);
        return 1;
    }
    printf("wrote %s\n", out);
    free(img);
    return 0;
}
