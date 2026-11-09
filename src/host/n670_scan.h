/*
 * n670_scan.h - small session API over the SANE plustek backend, shared by
 * the n670scan CLI and the WIA microdriver.
 */
#ifndef N670_SCAN_H
#define N670_SCAN_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    N670_MODE_LINEART = 0,   /* 1 bit/pixel, SANE convention: 1 = black */
    N670_MODE_GRAY    = 1,   /* 8 bit/pixel                             */
    N670_MODE_COLOR   = 2    /* 24 bit/pixel, R,G,B order               */
} N670Mode;

typedef struct {
    N670Mode mode;
    int      dpi;            /* 50..600 (backend picks nearest)         */
    double   x_mm, y_mm;     /* top-left of scan window, mm             */
    double   w_mm, h_mm;     /* size of scan window, mm (<=0: full bed) */
    int      preview;        /* fast/low-quality preview pass           */
    int      brightness;     /* -100..100 %                             */
    int      contrast;       /* -100..100 %                             */
} N670Params;

typedef struct {
    int pixels_per_line;
    int bytes_per_line;
    int lines;               /* may be -1 if unknown */
    int depth;
    int channels;            /* 1 or 3 */
} N670Frame;

typedef struct N670Session N670Session;

/* bed size reported by the backend (mm) - valid after n670_open */
typedef struct { double w_mm, h_mm; int max_dpi; } N670Bed;

int  n670_init(void);                 /* sane_init; returns 0 on success */
void n670_exit(void);
/* list devices into buf as "name|vendor model\n" lines; returns count */
int  n670_list(char *buf, size_t buflen);
/* open first (or named) device; returns 0 on success */
int  n670_open(const char *devname, N670Session **out, N670Bed *bed);
int  n670_start(N670Session *s, const N670Params *p, N670Frame *f);
/* read up to max bytes; returns bytes read, 0 at end of frame, <0 error */
long n670_read(N670Session *s, unsigned char *buf, long max);
void n670_cancel(N670Session *s);
void n670_close(N670Session *s);
const char *n670_last_error(void);

#ifdef __cplusplus
}
#endif
#endif
