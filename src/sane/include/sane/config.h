/* config.h for the Windows x64 (MSVC) build of the SANE plustek backend.
 * Hand-written replacement for the autoconf-generated header. */
#ifndef SANE_WIN64_CONFIG_H
#define SANE_WIN64_CONFIG_H
#include "../../../win32/posix_compat.h"

#define PACKAGE         "sane-backends"
#define VERSION         "git-win64"
#define PACKAGE_STRING  "sane-backends git-win64"

#define HAVE_SIGPROCMASK 1   /* use our struct sigaction shim            */
#define HAVE_USB         1
#define HAVE_LIMITS_H    1
#define HAVE_STRDUP      1
#define HAVE_STRNDUP     0
/* no HAVE_SETITIMER: plustek falls back to its non-timer lamp handling  */
/* no USE_PTHREAD:    sanei_thread is implemented on Win32 threads       */

#endif
