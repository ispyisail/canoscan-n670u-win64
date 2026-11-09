/*
 * n670_win32.h - host-side hooks into the Win32 sanei layer.
 */
#ifndef N670_WIN32_H
#define N670_WIN32_H

#ifdef __cplusplus
extern "C" {
#endif

/* Restrict sanei_usb to a single port (e.g. the one WIA hands the
 * microdriver). 'handle' may be NULL/INVALID_HANDLE_VALUE, in which case the
 * port is opened with CreateFile; otherwise that (borrowed) handle is used
 * and never closed by us. vid/pid are used if the IOCTL query fails.
 * Pass name=NULL to go back to normal enumeration. */
void n670_usb_set_port(const char *name, void *handle,
                       unsigned short vid, unsigned short pid);

/* Logging: level is the SANE debug level (0 = off, 255 = everything).
 * path NULL -> stderr. Overrides SANE_DEBUG_* env vars when level >= 0. */
void n670_set_log(const char *path, int level);
void n670_log(const char *fmt, ...);

/* Default tone curve applied by the backend's lookup tables.
 * 1.0 = linear (upstream SANE). Values < 1 darken midtones / add colour
 * depth. Clamped to 0.3..3.0. Must be set before n670_init(). */
void   n670_set_gamma(double gamma);
double n670_get_gamma(void);

/* Directory searched for plustek.conf and used for calibration cache. */
void n670_set_data_dir(const char *dir);
const char *n670_get_data_dir(void);

#ifdef __cplusplus
}
#endif
#endif
