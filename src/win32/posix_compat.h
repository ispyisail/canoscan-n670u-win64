/*
 * posix_compat.h - minimal POSIX shim so the SANE plustek backend builds
 * with MSVC for Windows x64. Force-included (/FI) into every SANE source.
 *
 * Only what the plustek USB backend + sanei_lm983x actually use is provided.
 */
#ifndef N670_POSIX_COMPAT_H
#define N670_POSIX_COMPAT_H

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef _CRT_NONSTDC_NO_WARNINGS
#define _CRT_NONSTDC_NO_WARNINGS
#endif
#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <io.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <malloc.h>
#include <process.h>
#include <limits.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- BSD-style integer types ---------------------------------------- */
typedef unsigned char  u_char;
typedef unsigned short u_short;
typedef unsigned int   u_int;
typedef unsigned long  u_long;    /* 32-bit on Win64; plustek only uses it
                                     for sizes/counters, never pointers   */
typedef unsigned short ushort;
typedef unsigned long  ulong;
typedef int            pid_t;
#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef intptr_t       ssize_t;
#endif

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

/* ---- strings --------------------------------------------------------- */
#define strcasecmp  _stricmp
#define strncasecmp _strnicmp

/* ---- time ------------------------------------------------------------ */
#ifndef _TIMEVAL_DEFINED
#define _TIMEVAL_DEFINED
struct timeval { long tv_sec; long tv_usec; };
#endif
int          n670_gettimeofday(struct timeval *tv, void *tz);
int          n670_usleep(unsigned long usec);
unsigned int n670_sleep(unsigned int sec);
#define gettimeofday n670_gettimeofday
#define usleep       n670_usleep
#define sleep        n670_sleep
#define alarm(x)     ((unsigned int)0)

/* ---- signals: the backend runs its reader as a thread, so all of the
 *      signal plumbing is a no-op; cancellation uses the cancelRead flag. */
#ifndef SIGHUP
#define SIGHUP  1
#endif
#ifndef SIGKILL
#define SIGKILL 9
#endif
#ifndef SIGUSR1
#define SIGUSR1 10
#endif
#ifndef SIGUSR2
#define SIGUSR2 12
#endif
#ifndef SIGPIPE
#define SIGPIPE 13
#endif
#ifndef SIGALRM
#define SIGALRM 14
#endif
#ifndef SIGCHLD
#define SIGCHLD 17
#endif

typedef unsigned long sigset_t;
struct sigaction {
    void   (*sa_handler)(int);
    sigset_t sa_mask;
    int      sa_flags;
};
#define SIG_BLOCK   1
#define SIG_UNBLOCK 2
#define SIG_SETMASK 3
#define sigemptyset(s)    (*(s) = 0, 0)
#define sigfillset(s)     (*(s) = ~0UL, 0)
#define sigaddset(s,n)    (*(s) |= (1UL << ((n) & 31)), 0)
#define sigdelset(s,n)    (*(s) &= ~(1UL << ((n) & 31)), 0)
#define sigprocmask(h,s,o) (0)
int n670_sigaction(int sig, const struct sigaction *act, struct sigaction *old);
#define sigaction(sig,act,old) n670_sigaction((sig),(act),(old))
/* MSVC's signal() aborts on unknown signal numbers (SIGCHLD...): stub it */
void (*n670_signal(int sig, void (*h)(int)))(int);
#define signal(sig,h) n670_signal((sig),(h))

/* ---- pipes / fds ----------------------------------------------------- */
#define pipe(fds) _pipe((fds), 4 * 1024 * 1024, _O_BINARY)
#ifndef O_NONBLOCK
#define O_NONBLOCK 0x4000
#endif
#define F_GETFL 3
#define F_SETFL 4
int n670_fcntl(int fd, int cmd, ...);
#define fcntl n670_fcntl

#ifdef __cplusplus
}
#endif

#endif /* N670_POSIX_COMPAT_H */
