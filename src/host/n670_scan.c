/*
 * n670_scan.c - session API over the SANE plustek backend.
 */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "sane/sane.h"
#include "n670_scan.h"
#include "n670_win32.h"

/* the backend's entry points, renamed by sanei_backend.h (ENTRY macro) */
extern SANE_Status sane_plustek_init(SANE_Int *, SANE_Auth_Callback);
extern void        sane_plustek_exit(void);
extern SANE_Status sane_plustek_get_devices(const SANE_Device ***, SANE_Bool);
extern SANE_Status sane_plustek_open(SANE_String_Const, SANE_Handle *);
extern void        sane_plustek_close(SANE_Handle);
extern const SANE_Option_Descriptor *
                   sane_plustek_get_option_descriptor(SANE_Handle, SANE_Int);
extern SANE_Status sane_plustek_control_option(SANE_Handle, SANE_Int,
                                               SANE_Action, void *, SANE_Int *);
extern SANE_Status sane_plustek_get_parameters(SANE_Handle, SANE_Parameters *);
extern SANE_Status sane_plustek_start(SANE_Handle);
extern SANE_Status sane_plustek_read(SANE_Handle, SANE_Byte *, SANE_Int, SANE_Int *);
extern void        sane_plustek_cancel(SANE_Handle);

struct N670Session {
    SANE_Handle h;
    int         scanning;
    int         eof;
};

static char g_err[256];
static int  g_inited;

static void set_err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_err, sizeof(g_err), fmt, ap);
    va_end(ap);
    n670_log("error: %s\n", g_err);
}

const char *n670_last_error(void) { return g_err; }

static const char *status_str(SANE_Status st)
{
    switch (st) {
    case SANE_STATUS_GOOD:          return "ok";
    case SANE_STATUS_UNSUPPORTED:   return "unsupported";
    case SANE_STATUS_CANCELLED:     return "cancelled";
    case SANE_STATUS_DEVICE_BUSY:   return "device busy";
    case SANE_STATUS_INVAL:         return "invalid argument";
    case SANE_STATUS_EOF:           return "end of file";
    case SANE_STATUS_JAMMED:        return "jammed";
    case SANE_STATUS_NO_DOCS:       return "no documents";
    case SANE_STATUS_COVER_OPEN:    return "cover open";
    case SANE_STATUS_IO_ERROR:      return "I/O error";
    case SANE_STATUS_NO_MEM:        return "out of memory";
    case SANE_STATUS_ACCESS_DENIED: return "access denied";
    default:                        return "unknown";
    }
}

int n670_init(void)
{
    SANE_Int ver = 0;
    SANE_Status st;
    if (g_inited)
        return 0;
    st = sane_plustek_init(&ver, NULL);
    if (st != SANE_STATUS_GOOD) {
        set_err("sane_init: %s", status_str(st));
        return -1;
    }
    g_inited = 1;
    return 0;
}

void n670_exit(void)
{
    if (g_inited) {
        sane_plustek_exit();
        g_inited = 0;
    }
}

int n670_list(char *buf, size_t buflen)
{
    const SANE_Device **list = NULL;
    size_t used = 0;
    int i, n = 0;

    if (buf && buflen) buf[0] = '\0';
    if (sane_plustek_get_devices(&list, SANE_TRUE) != SANE_STATUS_GOOD || !list)
        return 0;
    for (i = 0; list[i]; i++, n++) {
        if (buf && used < buflen)
            used += (size_t)snprintf(buf + used, buflen - used, "%s|%s %s\n",
                                     list[i]->name, list[i]->vendor,
                                     list[i]->model);
    }
    return n;
}

static int find_opt(SANE_Handle h, const char *name)
{
    const SANE_Option_Descriptor *d;
    SANE_Int count = 0, i;

    if (sane_plustek_control_option(h, 0, SANE_ACTION_GET_VALUE, &count, NULL)
        != SANE_STATUS_GOOD)
        return -1;
    for (i = 1; i < count; i++) {
        d = sane_plustek_get_option_descriptor(h, i);
        if (d && d->name && strcmp(d->name, name) == 0)
            return i;
    }
    return -1;
}

static int set_word(SANE_Handle h, const char *name, SANE_Word w)
{
    SANE_Int info = 0;
    int i = find_opt(h, name);
    SANE_Status st;
    if (i < 0) {
        n670_log("option '%s' not available\n", name);
        return -1;
    }
    st = sane_plustek_control_option(h, i, SANE_ACTION_SET_VALUE, &w, &info);
    if (st != SANE_STATUS_GOOD) {
        n670_log("set '%s' failed: %s\n", name, status_str(st));
        return -1;
    }
    return 0;
}

static int set_str(SANE_Handle h, const char *name, const char *v)
{
    char tmp[64];
    SANE_Int info = 0;
    int i = find_opt(h, name);
    SANE_Status st;
    if (i < 0)
        return -1;
    strncpy(tmp, v, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    st = sane_plustek_control_option(h, i, SANE_ACTION_SET_VALUE, tmp, &info);
    if (st != SANE_STATUS_GOOD) {
        n670_log("set '%s'='%s' failed: %s\n", name, v, status_str(st));
        return -1;
    }
    return 0;
}

static double range_max_mm(SANE_Handle h, const char *name, double dflt)
{
    int i = find_opt(h, name);
    const SANE_Option_Descriptor *d;
    if (i < 0)
        return dflt;
    d = sane_plustek_get_option_descriptor(h, i);
    if (d && d->constraint_type == SANE_CONSTRAINT_RANGE && d->constraint.range)
        return SANE_UNFIX(d->constraint.range->max);
    return dflt;
}

int n670_open(const char *devname, N670Session **out, N670Bed *bed)
{
    const SANE_Device **list = NULL;
    N670Session *s;
    SANE_Status st;
    const char *name = devname;

    *out = NULL;
    if (n670_init() != 0)
        return -1;

    if (!name || !*name) {
        st = sane_plustek_get_devices(&list, SANE_TRUE);
        if (st != SANE_STATUS_GOOD || !list || !list[0]) {
            set_err("no CanoScan N670U found (is the n670u driver bound to it?)");
            return -1;
        }
        name = list[0]->name;
    }

    s = (N670Session *)calloc(1, sizeof(*s));
    if (!s) {
        set_err("out of memory");
        return -1;
    }
    st = sane_plustek_open(name, &s->h);
    if (st != SANE_STATUS_GOOD) {
        set_err("sane_open(%s): %s", name, status_str(st));
        free(s);
        return -1;
    }
    if (bed) {
        int ri = find_opt(s->h, "resolution");
        const SANE_Option_Descriptor *d =
            ri >= 0 ? sane_plustek_get_option_descriptor(s->h, ri) : NULL;
        bed->w_mm = range_max_mm(s->h, "br-x", 215.0);
        bed->h_mm = range_max_mm(s->h, "br-y", 297.0);
        bed->max_dpi = 600;
        if (d && d->constraint_type == SANE_CONSTRAINT_RANGE && d->constraint.range)
            bed->max_dpi = d->constraint.range->max;
    }
    *out = s;
    return 0;
}

int n670_start(N670Session *s, const N670Params *p, N670Frame *f)
{
    SANE_Parameters sp;
    SANE_Status st;
    const char *mode;
    double bw = range_max_mm(s->h, "br-x", 215.0);
    double bh = range_max_mm(s->h, "br-y", 297.0);
    double x0 = p->x_mm, y0 = p->y_mm, x1, y1;

    switch (p->mode) {
    case N670_MODE_LINEART: mode = "Lineart"; break;
    case N670_MODE_GRAY:    mode = "Gray";    break;
    default:                mode = "Color";   break;
    }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    x1 = (p->w_mm > 0) ? x0 + p->w_mm : bw;
    y1 = (p->h_mm > 0) ? y0 + p->h_mm : bh;
    if (x1 > bw) x1 = bw;
    if (y1 > bh) y1 = bh;

    if (set_str(s->h, "mode", mode) != 0) {
        set_err("cannot set mode %s", mode);
        return -1;
    }
    if (p->mode != N670_MODE_LINEART)
        set_word(s->h, "depth", 8);
    set_word(s->h, "resolution", p->dpi > 0 ? p->dpi : 300);
    set_word(s->h, "preview", p->preview ? SANE_TRUE : SANE_FALSE);
    set_word(s->h, "brightness", SANE_FIX(p->brightness < -100 ? -100 :
                                          p->brightness > 100 ? 100 : p->brightness));
    set_word(s->h, "contrast",   SANE_FIX(p->contrast < -100 ? -100 :
                                          p->contrast > 100 ? 100 : p->contrast));
    /* set bottom-right first so top-left is never clamped by it */
    set_word(s->h, "br-x", SANE_FIX(x1));
    set_word(s->h, "br-y", SANE_FIX(y1));
    set_word(s->h, "tl-x", SANE_FIX(x0));
    set_word(s->h, "tl-y", SANE_FIX(y0));

    n670_log("start: mode=%s dpi=%d area=(%.1f,%.1f)-(%.1f,%.1f)mm\n",
             mode, p->dpi, x0, y0, x1, y1);

    st = sane_plustek_start(s->h);
    if (st != SANE_STATUS_GOOD) {
        set_err("sane_start: %s", status_str(st));
        return -1;
    }
    s->scanning = 1;
    s->eof = 0;

    st = sane_plustek_get_parameters(s->h, &sp);
    if (st != SANE_STATUS_GOOD) {
        set_err("sane_get_parameters: %s", status_str(st));
        n670_cancel(s);
        return -1;
    }
    f->pixels_per_line = sp.pixels_per_line;
    f->bytes_per_line  = sp.bytes_per_line;
    f->lines           = sp.lines;
    f->depth           = sp.depth;
    f->channels        = (sp.format == SANE_FRAME_RGB) ? 3 : 1;
    n670_log("frame: %d px/line, %d bytes/line, %d lines, depth %d, %d ch\n",
             f->pixels_per_line, f->bytes_per_line, f->lines, f->depth,
             f->channels);
    return 0;
}

long n670_read(N670Session *s, unsigned char *buf, long max)
{
    long total = 0;
    while (total < max && !s->eof) {
        SANE_Int got = 0;
        long want = max - total;
        SANE_Status st;
        if (want > 0x7fffffffL) want = 0x7fffffffL;
        st = sane_plustek_read(s->h, buf + total, (SANE_Int)want, &got);
        if (st == SANE_STATUS_EOF) {
            s->eof = 1;
            break;
        }
        if (st != SANE_STATUS_GOOD) {
            set_err("sane_read: %s", status_str(st));
            s->scanning = 0;
            return -1;
        }
        total += got;
    }
    return total;
}

void n670_cancel(N670Session *s)
{
    if (s && s->scanning) {
        sane_plustek_cancel(s->h);
        s->scanning = 0;
    }
}

void n670_close(N670Session *s)
{
    if (!s)
        return;
    n670_cancel(s);
    sane_plustek_close(s->h);
    free(s);
}
