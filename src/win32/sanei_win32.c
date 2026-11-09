/*
 * sanei_win32.c - Windows (x64) implementation of the sanei pieces the
 * plustek backend needs:
 *
 *   sanei_usb_*     -> usbscan.sys (Microsoft's signed still-image USB
 *                      driver): ReadFile = bulk-in, WriteFile = bulk-out
 *   sanei_thread_*  -> Win32 threads (the reader "process" runs as a thread)
 *   sanei_access_*  -> no-op (one process owns the device)
 *   sanei_config_*  -> plustek.conf next to the module / in the data dir
 *   sanei debug     -> log file or stderr
 *   POSIX helpers   -> usleep, gettimeofday, sigaction/signal/fcntl stubs
 */
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <setupapi.h>
#include <initguid.h>
#include <winioctl.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>

#include "posix_compat.h"
#include "n670_win32.h"

#include "sane/sane.h"
#include "sane/sanei.h"
#include "sane/sanei_usb.h"
#include "sane/sanei_thread.h"
#include "sane/sanei_access.h"
#include "sane/sanei_config.h"

#pragma comment(lib, "setupapi.lib")

/* ===================================================================== */
/* usbscan.sys interface (from the DDK's usbscan.h)                      */
/* ===================================================================== */
#define FILE_DEVICE_USB_SCAN 0x8000
#define USBSCAN_IOCTL_INDEX  0x0800
#define IOCTL_GET_DEVICE_DESCRIPTOR \
    CTL_CODE(FILE_DEVICE_USB_SCAN, USBSCAN_IOCTL_INDEX + 6, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_RESET_PIPE \
    CTL_CODE(FILE_DEVICE_USB_SCAN, USBSCAN_IOCTL_INDEX + 7, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_SET_TIMEOUT \
    CTL_CODE(FILE_DEVICE_USB_SCAN, USBSCAN_IOCTL_INDEX + 11, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct {
    USHORT usVendorId;
    USHORT usProductId;
    USHORT usBcdDevice;
    USHORT usLanguageId;
} USBSCAN_DEVICE_DESCRIPTOR;

typedef struct {
    ULONG TimeoutRead;   /* seconds */
    ULONG TimeoutWrite;
    ULONG TimeoutEvent;
} USBSCAN_TIMEOUT;

/* GUID_DEVINTERFACE_IMAGE - registered by usbscan.sys for every still-image
 * device it drives. */
DEFINE_GUID(N670_GUID_DEVINTERFACE_IMAGE,
    0x6bdd1fc6, 0x810f, 0x11d0, 0xbe, 0xc7, 0x08, 0x00, 0x2b, 0xe2, 0x09, 0x2f);

/* ===================================================================== */
/* logging                                                               */
/* ===================================================================== */
static CRITICAL_SECTION g_log_cs;
static INIT_ONCE        g_init_once = INIT_ONCE_STATIC_INIT;
static FILE            *g_log_fp = NULL;
static int              g_log_level_override = -1;
static char             g_data_dir[MAX_PATH] = "";

static BOOL CALLBACK n670_init_once(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o; (void)p; (void)c;
    InitializeCriticalSection(&g_log_cs);
    return TRUE;
}
static void n670_ensure_init(void)
{
    InitOnceExecuteOnce(&g_init_once, n670_init_once, NULL, NULL);
}

void n670_set_log(const char *path, int level)
{
    n670_ensure_init();
    EnterCriticalSection(&g_log_cs);
    if (g_log_fp && g_log_fp != stderr)
        fclose(g_log_fp);
    g_log_fp = NULL;
    if (path && *path)
        g_log_fp = _fsopen(path, "a", _SH_DENYWR);
    g_log_level_override = level;
    LeaveCriticalSection(&g_log_cs);
}

static void n670_vlog(const char *prefix, const char *fmt, va_list ap)
{
    FILE *fp;
    SYSTEMTIME st;
    n670_ensure_init();
    EnterCriticalSection(&g_log_cs);
    fp = g_log_fp ? g_log_fp : stderr;
    GetLocalTime(&st);
    fprintf(fp, "%02u:%02u:%02u.%03u [%5lu] %s",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            GetCurrentThreadId(), prefix);
    vfprintf(fp, fmt, ap);
    fflush(fp);
    LeaveCriticalSection(&g_log_cs);
}

void n670_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    n670_vlog("[n670] ", fmt, ap);
    va_end(ap);
}

/* SANE debug hooks ---------------------------------------------------- */
void sanei_init_debug(const char *backend, int *var)
{
    char name[128], *p;
    const char *val;

    if (g_log_level_override >= 0) {
        *var = g_log_level_override;
        return;
    }
    snprintf(name, sizeof(name), "SANE_DEBUG_%s", backend);
    for (p = name; *p; p++)
        *p = (char)toupper((unsigned char)*p);
    val = getenv(name);
    *var = val ? atoi(val) : 0;
}

void sanei_debug_msg(int level, int max_level, const char *be,
                     const char *fmt, va_list ap)
{
    char prefix[64];
    if (max_level < level)
        return;
    snprintf(prefix, sizeof(prefix), "[%s] ", be);
    n670_vlog(prefix, fmt, ap);
}

void sanei_debug_ndebug(int level, const char *msg, ...)
{
    (void)level; (void)msg;
}

/* ===================================================================== */
/* default gamma                                                         */
/* ===================================================================== */
static double g_gamma = 1.0;   /* linear, as upstream SANE (0.9 was tried and
                                  judged too dark on real scans)         */

void n670_set_gamma(double gamma)
{
    if (gamma < 0.3) gamma = 0.3;
    if (gamma > 3.0) gamma = 3.0;
    g_gamma = gamma;
}

double n670_get_gamma(void) { return g_gamma; }

/* ===================================================================== */
/* data directory / config                                               */
/* ===================================================================== */
void n670_set_data_dir(const char *dir)
{
    if (dir)
        strncpy(g_data_dir, dir, sizeof(g_data_dir) - 1);
    else
        g_data_dir[0] = '\0';
}

const char *n670_get_data_dir(void)
{
    if (!g_data_dir[0]) {
        DWORD n = GetTempPathA(sizeof(g_data_dir), g_data_dir);
        if (n == 0 || n >= sizeof(g_data_dir))
            strcpy(g_data_dir, ".");
        else if (g_data_dir[n - 1] == '\\')
            g_data_dir[n - 1] = '\0';
    }
    return g_data_dir;
}

static void module_dir(char *out, size_t n)
{
    HMODULE self = NULL;
    char *slash;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&module_dir, &self);
    out[0] = '\0';
    GetModuleFileNameA(self, out, (DWORD)n);
    slash = strrchr(out, '\\');
    if (slash) *slash = '\0';
}

FILE *sanei_config_open(const char *name)
{
    char path[MAX_PATH * 2];
    FILE *fp;

    snprintf(path, sizeof(path), "%s\\%s", n670_get_data_dir(), name);
    if ((fp = fopen(path, "r")) != NULL)
        return fp;
    module_dir(path, MAX_PATH);
    strncat(path, "\\", sizeof(path) - strlen(path) - 1);
    strncat(path, name, sizeof(path) - strlen(path) - 1);
    return fopen(path, "r");
}

char *sanei_config_read(char *str, int n, FILE *stream)
{
    char *rc, *start;
    size_t len;

    rc = fgets(str, n, stream);
    if (!rc)
        return NULL;
    len = strlen(str);
    while (len > 0 && isspace((unsigned char)str[len - 1]))
        str[--len] = '\0';
    start = str;
    while (isspace((unsigned char)*start))
        start++;
    if (start != str)
        memmove(str, start, strlen(start) + 1);
    return rc;
}

const char *sanei_config_skip_whitespace(const char *str)
{
    while (str && *str && isspace((unsigned char)*str))
        ++str;
    return str;
}

const char *sanei_config_get_string(const char *str, char **string_const)
{
    const char *start;
    size_t len;

    str = sanei_config_skip_whitespace(str);
    if (*str == '"') {
        start = ++str;
        while (*str && *str != '"')
            ++str;
        len = (size_t)(str - start);
        if (*str == '"')
            ++str;
        else
            start = NULL;
    } else {
        start = str;
        while (*str && !isspace((unsigned char)*str))
            ++str;
        len = (size_t)(str - start);
    }
    if (start) {
        *string_const = (char *)malloc(len + 1);
        if (*string_const) {
            memcpy(*string_const, start, len);
            (*string_const)[len] = '\0';
        }
    } else {
        *string_const = NULL;
    }
    return str;
}

/* ===================================================================== */
/* sanei_access - machine-wide lock so the WIA service and n670scan.exe  */
/* never drive the LM983x at the same time (that wedges the chip).       */
/* A semaphore, not a mutex: plustek may lock and unlock on different    */
/* threads. Global\ when we may create it, else Local\ (best effort).    */
/* ===================================================================== */
#include <sddl.h>
#pragma comment(lib, "advapi32.lib")

static HANDLE g_access_sem;
static int    g_access_depth;

static HANDLE access_sem(void)
{
    SECURITY_ATTRIBUTES sa;
    PSECURITY_DESCRIPTOR sd = NULL;
    HANDLE h;

    if (g_access_sem)
        return g_access_sem;
    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    /* everyone (incl. LocalService + interactive user) may use it */
    if (ConvertStringSecurityDescriptorToSecurityDescriptorA(
            "D:(A;;GA;;;WD)(A;;GA;;;SY)", SDDL_REVISION_1, &sd, NULL))
        sa.lpSecurityDescriptor = sd;
    h = CreateSemaphoreA(&sa, 1, 1, "Global\\N670U_ScannerAccess");
    if (!h)
        h = OpenSemaphoreA(SEMAPHORE_ALL_ACCESS, FALSE, "Global\\N670U_ScannerAccess");
    if (!h)
        h = CreateSemaphoreA(&sa, 1, 1, "Local\\N670U_ScannerAccess");
    if (sd)
        LocalFree(sd);
    g_access_sem = h;
    if (!h)
        n670_log("access: no lock object (error %lu), continuing unlocked\n",
                 GetLastError());
    return h;
}

void sanei_access_init(const char *backend) { (void)backend; access_sem(); }

SANE_Status sanei_access_lock(const char *devicename, SANE_Word timeout)
{
    HANDLE h = access_sem();
    DWORD rc;
    (void)devicename;
    if (!h)
        return SANE_STATUS_GOOD;
    if (g_access_depth > 0) {          /* already ours (nested open) */
        g_access_depth++;
        return SANE_STATUS_GOOD;
    }
    rc = WaitForSingleObject(h, (DWORD)(timeout > 0 ? timeout : 1) * 1000);
    if (rc != WAIT_OBJECT_0) {
        n670_log("access: scanner is busy in another program\n");
        return SANE_STATUS_DEVICE_BUSY;
    }
    g_access_depth = 1;
    return SANE_STATUS_GOOD;
}

SANE_Status sanei_access_unlock(const char *devicename)
{
    (void)devicename;
    if (g_access_sem && g_access_depth > 0 && --g_access_depth == 0)
        ReleaseSemaphore(g_access_sem, 1, NULL);
    return SANE_STATUS_GOOD;
}

/* ===================================================================== */
/* sanei_usb over usbscan.sys                                            */
/* ===================================================================== */
#define MAX_USB_DEVS   8
#define MAX_USB_OPEN   8
#define USB_TIMEOUT_MS 30000

typedef struct {
    char   name[512];
    USHORT vid, pid;
    HANDLE borrowed;      /* handle supplied by the WIA host, or NULL */
} UsbDev;

typedef struct {
    BOOL   used;
    HANDLE h;
    BOOL   owned;         /* close on sanei_usb_close? */
    int    dev;           /* index into g_devs */
    HANDLE ev;            /* overlapped event */
} UsbOpen;

static UsbDev  g_devs[MAX_USB_DEVS];
static int     g_ndevs;
static UsbOpen g_open[MAX_USB_OPEN];
static DWORD   g_usb_timeout = USB_TIMEOUT_MS;

static char    g_forced_name[512];
static HANDLE  g_forced_handle;
static USHORT  g_forced_vid, g_forced_pid;

void n670_usb_set_port(const char *name, void *handle,
                       unsigned short vid, unsigned short pid)
{
    if (name) {
        strncpy(g_forced_name, name, sizeof(g_forced_name) - 1);
        g_forced_name[sizeof(g_forced_name) - 1] = '\0';
    } else {
        g_forced_name[0] = '\0';
    }
    g_forced_handle = (handle == INVALID_HANDLE_VALUE) ? NULL : (HANDLE)handle;
    g_forced_vid = vid;
    g_forced_pid = pid;
}

static HANDLE usb_create(const char *name)
{
    return CreateFileA(name, GENERIC_READ | GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                       OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
}

/* Synchronous IOCTL that works on both overlapped and non-overlapped
 * handles. */
static BOOL usb_ioctl(HANDLE h, DWORD code, void *in, DWORD inlen,
                      void *out, DWORD outlen, DWORD *ret)
{
    OVERLAPPED ov;
    BOOL ok;
    DWORD dummy = 0;
    if (!ret) ret = &dummy;
    memset(&ov, 0, sizeof(ov));
    ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    ok = DeviceIoControl(h, code, in, inlen, out, outlen, ret, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING)
        ok = GetOverlappedResult(h, &ov, ret, TRUE);
    CloseHandle(ov.hEvent);
    return ok;
}

/* parse "vid_04a9&pid_220d" from a device path, case-insensitively */
static BOOL parse_vid_pid(const char *path, USHORT *vid, USHORT *pid)
{
    char low[512];
    const char *v, *p;
    size_t i;
    for (i = 0; path[i] && i < sizeof(low) - 1; i++)
        low[i] = (char)tolower((unsigned char)path[i]);
    low[i] = '\0';
    v = strstr(low, "vid_");
    p = strstr(low, "pid_");
    if (!v || !p)
        return FALSE;
    *vid = (USHORT)strtoul(v + 4, NULL, 16);
    *pid = (USHORT)strtoul(p + 4, NULL, 16);
    return TRUE;
}

static BOOL query_ids(HANDLE h, const char *name, USHORT *vid, USHORT *pid)
{
    USBSCAN_DEVICE_DESCRIPTOR dd;
    DWORD ret = 0;
    memset(&dd, 0, sizeof(dd));
    if (h && h != INVALID_HANDLE_VALUE &&
        usb_ioctl(h, IOCTL_GET_DEVICE_DESCRIPTOR, NULL, 0, &dd, sizeof(dd), &ret) &&
        ret >= 4 && dd.usVendorId) {
        *vid = dd.usVendorId;
        *pid = dd.usProductId;
        return TRUE;
    }
    return parse_vid_pid(name, vid, pid);
}

static void add_dev(const char *name, USHORT vid, USHORT pid, HANDLE borrowed)
{
    int i;
    if (g_ndevs >= MAX_USB_DEVS)
        return;
    for (i = 0; i < g_ndevs; i++)
        if (_stricmp(g_devs[i].name, name) == 0)
            return;
    strncpy(g_devs[g_ndevs].name, name, sizeof(g_devs[0].name) - 1);
    g_devs[g_ndevs].vid = vid;
    g_devs[g_ndevs].pid = pid;
    g_devs[g_ndevs].borrowed = borrowed;
    n670_log("usb: found %s (%04x:%04x)%s\n", name, vid, pid,
             borrowed ? " [host handle]" : "");
    g_ndevs++;
}

void sanei_usb_scan_devices(void)
{
    HDEVINFO di;
    SP_DEVICE_INTERFACE_DATA ifd;
    DWORD idx, need;
    int n;

    g_ndevs = 0;

    if (g_forced_name[0]) {
        USHORT vid = g_forced_vid, pid = g_forced_pid;
        HANDLE h = g_forced_handle;
        BOOL tmp = FALSE;
        if (!h) {
            h = usb_create(g_forced_name);
            tmp = (h != INVALID_HANDLE_VALUE);
        }
        if (!query_ids(h, g_forced_name, &vid, &pid)) {
            vid = g_forced_vid;
            pid = g_forced_pid;
        }
        if (tmp)
            CloseHandle(h);
        add_dev(g_forced_name, vid, pid, g_forced_handle);
        return;
    }

    /* 1) every still-image device interface (usbscan registers these) */
    di = SetupDiGetClassDevsA(&N670_GUID_DEVINTERFACE_IMAGE, NULL, NULL,
                              DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (di != INVALID_HANDLE_VALUE) {
        ifd.cbSize = sizeof(ifd);
        for (idx = 0; SetupDiEnumDeviceInterfaces(di, NULL,
                     &N670_GUID_DEVINTERFACE_IMAGE, idx, &ifd); idx++) {
            PSP_DEVICE_INTERFACE_DETAIL_DATA_A det;
            need = 0;
            SetupDiGetDeviceInterfaceDetailA(di, &ifd, NULL, 0, &need, NULL);
            if (!need)
                continue;
            det = (PSP_DEVICE_INTERFACE_DETAIL_DATA_A)malloc(need);
            if (!det)
                continue;
            det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
            if (SetupDiGetDeviceInterfaceDetailA(di, &ifd, det, need, NULL, NULL)) {
                USHORT vid = 0, pid = 0;
                HANDLE h = usb_create(det->DevicePath);
                if (h != INVALID_HANDLE_VALUE) {
                    if (query_ids(h, det->DevicePath, &vid, &pid))
                        add_dev(det->DevicePath, vid, pid, NULL);
                    CloseHandle(h);
                } else if (parse_vid_pid(det->DevicePath, &vid, &pid)) {
                    /* busy (e.g. held by WIA) - still list it */
                    add_dev(det->DevicePath, vid, pid, NULL);
                }
            }
            free(det);
        }
        SetupDiDestroyDeviceInfoList(di);
    }

    /* 2) legacy \\.\UsbscanN names */
    if (g_ndevs == 0) {
        for (n = 0; n < 16; n++) {
            char name[32];
            HANDLE h;
            USHORT vid = 0, pid = 0;
            snprintf(name, sizeof(name), "\\\\.\\Usbscan%d", n);
            h = usb_create(name);
            if (h == INVALID_HANDLE_VALUE)
                continue;
            if (query_ids(h, name, &vid, &pid))
                add_dev(name, vid, pid, NULL);
            CloseHandle(h);
        }
    }
}

void sanei_usb_init(void)
{
    n670_ensure_init();
    sanei_usb_scan_devices();
}

void sanei_usb_exit(void)
{
    int i;
    for (i = 0; i < MAX_USB_OPEN; i++)
        if (g_open[i].used)
            sanei_usb_close(i);
    g_ndevs = 0;
}

SANE_Status sanei_usb_find_devices(SANE_Int vendor, SANE_Int product,
                                   SANE_Status (*attach)(SANE_String_Const devname))
{
    int i;
    for (i = 0; i < g_ndevs; i++) {
        if (g_devs[i].vid == (USHORT)vendor && g_devs[i].pid == (USHORT)product) {
            if (attach)
                attach(g_devs[i].name);
        }
    }
    return SANE_STATUS_GOOD;
}

SANE_Status sanei_usb_open(SANE_String_Const devname, SANE_Int *dn)
{
    int d, o;
    HANDLE h;
    BOOL owned;

    for (d = 0; d < g_ndevs; d++)
        if (_stricmp(g_devs[d].name, devname) == 0)
            break;
    if (d == g_ndevs) {
        n670_log("usb: open(%s): unknown device\n", devname);
        return SANE_STATUS_INVAL;
    }
    for (o = 0; o < MAX_USB_OPEN; o++)
        if (!g_open[o].used)
            break;
    if (o == MAX_USB_OPEN)
        return SANE_STATUS_NO_MEM;

    if (g_devs[d].borrowed) {
        h = g_devs[d].borrowed;
        owned = FALSE;
    } else {
        h = usb_create(devname);
        if (h == INVALID_HANDLE_VALUE) {
            DWORD err = GetLastError();
            n670_log("usb: CreateFile(%s) failed, error %lu\n", devname, err);
            errno = (err == ERROR_ACCESS_DENIED || err == ERROR_SHARING_VIOLATION)
                        ? EBUSY : ENODEV;
            return (errno == EBUSY) ? SANE_STATUS_DEVICE_BUSY
                                    : SANE_STATUS_ACCESS_DENIED;
        }
        owned = TRUE;
        {
            /* usbscan default timeouts are long; make reads fail sooner
             * than our own overlapped wait so errors surface cleanly */
            USBSCAN_TIMEOUT to = { 60, 60, 0 };
            usb_ioctl(h, IOCTL_SET_TIMEOUT, &to, sizeof(to), NULL, 0, NULL);
        }
    }

    /* Clear any stall left on the bulk pipes (seen on the first access
     * after power-up: ReadFile fails with ERROR_GEN_FAILURE otherwise). */
    {
        DWORD pipe = 3; /* ALL_PIPE */
        if (!usb_ioctl(h, IOCTL_RESET_PIPE, &pipe, sizeof(pipe), NULL, 0, NULL))
            n670_log("usb: reset pipes on open failed, error %lu\n", GetLastError());
    }

    g_open[o].used  = TRUE;
    g_open[o].h     = h;
    g_open[o].owned = owned;
    g_open[o].dev   = d;
    g_open[o].ev    = CreateEventA(NULL, TRUE, FALSE, NULL);
    *dn = o;
    return SANE_STATUS_GOOD;
}

void sanei_usb_close(SANE_Int dn)
{
    if (dn < 0 || dn >= MAX_USB_OPEN || !g_open[dn].used)
        return;
    if (g_open[dn].owned)
        CloseHandle(g_open[dn].h);
    if (g_open[dn].ev)
        CloseHandle(g_open[dn].ev);
    memset(&g_open[dn], 0, sizeof(g_open[dn]));
}

void sanei_usb_set_timeout(SANE_Int timeout)
{
    g_usb_timeout = timeout > 0 ? (DWORD)timeout : USB_TIMEOUT_MS;
}

SANE_Status sanei_usb_get_vendor_product(SANE_Int dn, SANE_Word *vendor,
                                         SANE_Word *product)
{
    if (dn < 0 || dn >= MAX_USB_OPEN || !g_open[dn].used)
        return SANE_STATUS_INVAL;
    if (vendor)  *vendor  = g_devs[g_open[dn].dev].vid;
    if (product) *product = g_devs[g_open[dn].dev].pid;
    return SANE_STATUS_GOOD;
}

/* One overlapped transfer with a timeout. Returns TRUE on success. */
static BOOL usb_xfer_once(UsbOpen *u, BOOL wr, void *buf, DWORD len,
                          DWORD timeout_ms, DWORD *got, DWORD *err)
{
    OVERLAPPED ov;
    BOOL ok;

    *got = 0;
    *err = 0;
    memset(&ov, 0, sizeof(ov));
    ResetEvent(u->ev);
    ov.hEvent = u->ev;
    ok = wr ? WriteFile(u->h, buf, len, got, &ov)
            : ReadFile(u->h, buf, len, got, &ov);
    if (ok)
        return TRUE;
    *err = GetLastError();
    if (*err != ERROR_IO_PENDING)
        return FALSE;
    if (WaitForSingleObject(u->ev, timeout_ms) != WAIT_OBJECT_0) {
        CancelIoEx(u->h, &ov);
        GetOverlappedResult(u->h, &ov, got, TRUE);
        *err = ERROR_TIMEOUT;
        return FALSE;
    }
    ok = GetOverlappedResult(u->h, &ov, got, FALSE);
    if (!ok)
        *err = GetLastError();
    return ok;
}

/* Bring the LM983x command channel back after a failed transfer:
 * clear host/device stalls, then drain any reply the chip is still holding
 * (the chip accepts no new commands while unread IN data is pending). */
static void usb_recover(UsbOpen *u, BOOL drain)
{
    DWORD pipe = 3 /* ALL_PIPE */, got, err;
    BYTE junk[512];
    int i, drained = 0;

    usb_ioctl(u->h, IOCTL_RESET_PIPE, &pipe, sizeof(pipe), NULL, 0, NULL);
    n670_usleep(20000);
    if (drain) {
        for (i = 0; i < 8; i++) {
            if (!usb_xfer_once(u, FALSE, junk, sizeof(junk), 150, &got, &err) || got == 0)
                break;
            drained += (int)got;
        }
        if (drained)
            n670_log("usb: recovery drained %d stale byte(s)\n", drained);
        usb_ioctl(u->h, IOCTL_RESET_PIPE, &pipe, sizeof(pipe), NULL, 0, NULL);
    }
}

#define USB_CMD_TIMEOUT_MS 5000   /* register commands are tiny */
#define USB_WRITE_TRIES    3

static SANE_Status usb_xfer(SANE_Int dn, BOOL wr, void *buf, size_t *size)
{
    DWORD got = 0, err = 0, len, timeout;
    UsbOpen *u;
    int attempt, tries;

    if (dn < 0 || dn >= MAX_USB_OPEN || !g_open[dn].used || !size)
        return SANE_STATUS_INVAL;
    u = &g_open[dn];
    if (*size == 0)
        return SANE_STATUS_GOOD;
    len = (DWORD)*size;

    /* Writes are LM983x commands (+ register/gamma/shading payloads) and
     * are safe to resend after a stall. A failed read is retried once
     * without draining - draining would throw away real image data. */
    if (wr) {
        timeout = (len <= 4096) ? USB_CMD_TIMEOUT_MS : g_usb_timeout;
        tries   = USB_WRITE_TRIES;
    } else if (len <= 64) {
        /* register reply: arrives at once or not at all. One try only -
         * sanei_lm983x_read() resends the whole command on failure. */
        timeout = 3000;
        tries   = 1;
    } else {
        timeout = g_usb_timeout;
        tries   = 2;
    }

    for (attempt = 1; attempt <= tries; attempt++) {
        if (usb_xfer_once(u, wr, buf, len, timeout, &got, &err)) {
            if (attempt > 1)
                n670_log("usb: %s of %lu bytes OK on attempt %d\n",
                         wr ? "write" : "read", len, attempt);
            *size = got;
            return SANE_STATUS_GOOD;
        }
        n670_log("usb: %s of %lu bytes %s (error %lu), attempt %d/%d\n",
                 wr ? "write" : "read", len,
                 err == ERROR_TIMEOUT ? "timed out" : "failed",
                 err, attempt, tries);
        if (attempt < tries)
            usb_recover(u, wr);
    }
    *size = 0;
    return SANE_STATUS_IO_ERROR;
}

SANE_Status sanei_usb_read_bulk(SANE_Int dn, SANE_Byte *buffer, size_t *size)
{
    return usb_xfer(dn, FALSE, buffer, size);
}

SANE_Status sanei_usb_write_bulk(SANE_Int dn, const SANE_Byte *buffer, size_t *size)
{
    return usb_xfer(dn, TRUE, (void *)buffer, size);
}

/* ===================================================================== */
/* sanei_thread on Win32 threads                                         */
/* ===================================================================== */
#define MAX_THREADS 8

typedef struct {
    HANDLE h;
    int  (*func)(void *);
    void  *args;
    int    rc;
    BOOL   done;
} ThreadSlot;

static ThreadSlot g_threads[MAX_THREADS];

static DWORD WINAPI thread_tramp(LPVOID p)
{
    ThreadSlot *t = (ThreadSlot *)p;
    t->rc = t->func(t->args);
    t->done = TRUE;
    return (DWORD)t->rc;
}

void sanei_thread_init(void) { n670_ensure_init(); }

SANE_Bool sanei_thread_is_forked(void) { return SANE_FALSE; }

SANE_Bool sanei_thread_is_valid(SANE_Pid pid)
{
    return (pid >= 0 && pid < MAX_THREADS && g_threads[pid].h) ? SANE_TRUE
                                                                : SANE_FALSE;
}

SANE_Pid sanei_thread_begin(int (*func)(void *args), void *args)
{
    int i;
    for (i = 0; i < MAX_THREADS; i++)
        if (!g_threads[i].h)
            break;
    if (i == MAX_THREADS)
        return (SANE_Pid)-1;
    g_threads[i].func = func;
    g_threads[i].args = args;
    g_threads[i].rc   = SANE_STATUS_GOOD;
    g_threads[i].done = FALSE;
    g_threads[i].h    = CreateThread(NULL, 1024 * 1024, thread_tramp,
                                     &g_threads[i], 0, NULL);
    if (!g_threads[i].h)
        return (SANE_Pid)-1;
    return (SANE_Pid)i;
}

int sanei_thread_sendsig(SANE_Pid pid, int sig)
{
    (void)pid; (void)sig;   /* cancellation goes through cancelRead */
    return 0;
}

int sanei_thread_kill(SANE_Pid pid)
{
    /* never TerminateThread: it can leave the CRT/heap locked. The reader
     * always exits once cancelRead is set or the pipe is closed. */
    (void)pid;
    return 0;
}

SANE_Pid sanei_thread_waitpid(SANE_Pid pid, int *status)
{
    if (!sanei_thread_is_valid(pid))
        return (SANE_Pid)-1;
    WaitForSingleObject(g_threads[pid].h, INFINITE);
    if (status)
        *status = g_threads[pid].rc;
    CloseHandle(g_threads[pid].h);
    g_threads[pid].h = NULL;
    return pid;
}

SANE_Status sanei_thread_get_status(SANE_Pid pid)
{
    if (pid >= 0 && pid < MAX_THREADS)
        return (SANE_Status)g_threads[pid].rc;
    return SANE_STATUS_INVAL;
}

/* ===================================================================== */
/* POSIX helpers                                                         */
/* ===================================================================== */
int n670_gettimeofday(struct timeval *tv, void *tz)
{
    FILETIME ft;
    ULARGE_INTEGER u;
    (void)tz;
    GetSystemTimePreciseAsFileTime(&ft);
    u.LowPart  = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    u.QuadPart -= 116444736000000000ULL;          /* 1601 -> 1970 */
    tv->tv_sec  = (long)(u.QuadPart / 10000000ULL);
    tv->tv_usec = (long)((u.QuadPart % 10000000ULL) / 10ULL);
    return 0;
}

int n670_usleep(unsigned long usec)
{
    HANDLE t;
    LARGE_INTEGER due;
    if (usec == 0)
        return 0;
    t = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                               TIMER_ALL_ACCESS);
    if (!t) {
        Sleep((DWORD)((usec + 999) / 1000));
        return 0;
    }
    due.QuadPart = -(LONGLONG)usec * 10;          /* relative, 100ns units */
    SetWaitableTimer(t, &due, 0, NULL, NULL, FALSE);
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    return 0;
}

unsigned int n670_sleep(unsigned int sec)
{
    Sleep(sec * 1000);
    return 0;
}

int n670_sigaction(int sig, const struct sigaction *act, struct sigaction *old)
{
    (void)sig; (void)act;
    if (old)
        memset(old, 0, sizeof(*old));
    return 0;
}

void (*n670_signal(int sig, void (*h)(int)))(int)
{
    (void)sig; (void)h;
    return SIG_DFL;
}

int n670_fcntl(int fd, int cmd, ...)
{
    va_list ap;
    int arg = 0;
    (void)fd;
    va_start(ap, cmd);
    if (cmd == F_SETFL)
        arg = va_arg(ap, int);
    va_end(ap);
    if (cmd == F_SETFL && (arg & O_NONBLOCK)) {
        errno = ENOSYS;            /* non-blocking pipe reads unsupported */
        return -1;
    }
    return 0;
}
