/*
 * n670mdrv.cpp - WIA scanner microdriver for the Canon CanoScan
 * N670U / N676U / LiDE20 (USB 04A9:220D).
 *
 * Loaded by Microsoft's WIA flatbed driver (wiafbdrv.dll), which implements
 * all WIA properties/COM plumbing and calls the three exports below. The
 * actual scanner control is the SANE plustek backend, talking to the device
 * through usbscan.sys (see src/win32/sanei_win32.c).
 *
 * Data contract with wiafbdrv:
 *   - SetPixelWindow() gives the scan window in pixels at the current
 *     X/Y resolution. We report WidthPixels/WidthBytes/Lines for it.
 *   - Scan(SCAN_FIRST/SCAN_NEXT) must return exactly WidthBytes*Lines bytes
 *     in total, packed (unaligned, bNeedDataAlignment=TRUE), RGB order.
 *     SANE's frame can differ by a pixel or two from the requested window,
 *     so every SANE line is cropped/padded to the WIA line size and
 *     missing lines are padded with white.
 *   - Scan(SCAN_FINISHED) is always called at the end (also on cancel).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <sti.h>
#include <wiamicro.h>

extern "C" {
#include "n670_scan.h"
#include "n670_win32.h"
}

/* ------------------------------------------------------------------------ */
/* device constants (from the plustek backend's Canon 0x220D profile)        */
/* ------------------------------------------------------------------------ */
static const LONG   kOpticalDpi = 600;
static const LONG   kBedWidth   = 8500;   /* 1/1000 inch: 2550 px @ 300 dpi */
static const LONG   kBedHeight  = 11690;  /* 1/1000 inch: 3508 px @ 300 dpi */
static const USHORT kVid = 0x04A9, kPid = 0x220D;

/* ------------------------------------------------------------------------ */
/* driver state                                                              */
/* ------------------------------------------------------------------------ */
struct DrvCtx {
    CRITICAL_SECTION cs;
    char         port[MAX_ANSI_CHAR];
    HANDLE       borrowed;
    N670Session *sess;
    bool         preview;
    int          debugLevel;
    bool         whiteIsOne;      /* 1-bit output polarity for WIA        */

    /* per-scan */
    bool         scanning;
    N670Frame    frame;
    unsigned char *saneLine;      /* one SANE line                         */
    unsigned char *outLine;       /* one WIA line                          */
    LONG         outBpl;          /* WIA bytes per line                    */
    LONG         outLines;        /* WIA total lines                       */
    LONG         linesDone;       /* WIA lines fully emitted               */
    LONG         outPos;          /* bytes of outLine already emitted      */
    bool         haveOutLine;
    bool         saneEof;
    bool         negative;
    int          dataType;
};

static DrvCtx  g_ctx;
static bool    g_ctxInit;
static HMODULE g_hInst;

static void CtxInit()
{
    if (!g_ctxInit) {
        memset(&g_ctx, 0, sizeof(g_ctx));
        InitializeCriticalSection(&g_ctx.cs);
        g_ctx.whiteIsOne = true;
        g_ctxInit = true;
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_hInst = hModule;
        DisableThreadLibraryCalls(hModule);
        CtxInit();
    }
    return TRUE;
}

/* Trace() is declared by wiamicro.h */
VOID Trace(_In_ LPCTSTR format, ...)
{
    if (g_ctx.debugLevel <= 0)
        return;
    WCHAR wbuf[1024];
    char  buf[1024];
    va_list ap;
    va_start(ap, format);
    _vsnwprintf_s(wbuf, _countof(wbuf), _TRUNCATE, format, ap);
    va_end(ap);
    WideCharToMultiByte(CP_ACP, 0, wbuf, -1, buf, sizeof(buf), NULL, NULL);
    n670_log("wia: %s\n", buf);
}

/* ------------------------------------------------------------------------ */
/* helpers                                                                   */
/* ------------------------------------------------------------------------ */
static LONG BitsFor(LONG dataType)
{
    switch (dataType) {
    case WIA_DATA_THRESHOLD: return 1;
    case WIA_DATA_GRAYSCALE: return 8;
    default:                 return 24;
    }
}

static void UpdateImageInfo(PSCANINFO si)
{
    si->PixelBits   = BitsFor(si->DataType);
    si->WidthPixels = si->Window.xExtent;
    si->WidthBytes  = (si->Window.xExtent * si->PixelBits + 7) / 8;
    si->Lines       = si->Window.yExtent;
}

/* DeviceData values from the INF arrive as REG_SZ; ones set by hand or by
 * install.ps1 are REG_DWORD. Accept either. */
static bool RegGetNumber(HKEY h, const wchar_t *name, DWORD *out)
{
    BYTE  buf[64];
    DWORD cb = sizeof(buf) - sizeof(wchar_t), type = 0;
    memset(buf, 0, sizeof(buf));
    if (RegQueryValueExW(h, name, NULL, &type, buf, &cb) != ERROR_SUCCESS)
        return false;
    if (type == REG_DWORD && cb >= sizeof(DWORD)) {
        *out = *(DWORD *)buf;
        return true;
    }
    if (type == REG_SZ) {
        wchar_t *end = NULL;
        unsigned long v = wcstoul((const wchar_t *)buf, &end, 0);
        if (end != (const wchar_t *)buf) {
            *out = (DWORD)v;
            return true;
        }
    }
    return false;
}

static void ReadRegistry(HKEY hDevKey)
{
    HKEY hData = NULL;
    if (RegOpenKeyExW(hDevKey, L"DeviceData", 0, KEY_READ, &hData) != ERROR_SUCCESS)
        return;
    DWORD v;
    if (RegGetNumber(hData, L"DebugLevel", &v))
        g_ctx.debugLevel = (int)v;
    if (RegGetNumber(hData, L"LineartWhiteIsOne", &v))
        g_ctx.whiteIsOne = (v != 0);
    /* Gamma in percent: 100 = linear (default), 90 = darker/richer */
    if (RegGetNumber(hData, L"Gamma", &v) && v > 0)
        n670_set_gamma(v / 100.0);
    RegCloseKey(hData);
}

static void SetupLogging()
{
    char path[MAX_PATH];
    char pd[MAX_PATH];
    /* Prefer %ProgramData%\N670U (created by install.ps1 with an ACL that
     * lets the WIA service write and users read); else the service %TEMP% */
    DWORD n = GetEnvironmentVariableA("ProgramData", pd, sizeof(pd));
    if (n > 0 && n < sizeof(pd) - 8) {
        strcat_s(pd, sizeof(pd), "\\N670U");
        DWORD attr = GetFileAttributesA(pd);
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
            n670_set_data_dir(pd);
    }
    n670_get_data_dir();
    if (g_ctx.debugLevel > 0) {
        snprintf(path, sizeof(path), "%s\\n670u.log", n670_get_data_dir());
        n670_set_log(path, g_ctx.debugLevel);
        n670_log("---- n670mdrv loaded, port '%s', debug %d, gamma %.2f ----\n",
                 g_ctx.port, g_ctx.debugLevel, n670_get_gamma());
    } else {
        n670_set_log(NULL, 0);
    }
}

static void InitDefaults(PSCANINFO si)
{
    si->ADF                      = 0;
    si->TPA                      = 0;
    si->Endorser                 = 0;
    si->OpticalXResolution       = kOpticalDpi;
    si->OpticalYResolution       = kOpticalDpi;
    si->BedWidth                 = kBedWidth;
    si->BedHeight                = kBedHeight;

    /* map 1:1 onto SANE's -100..100 % brightness/contrast */
    si->IntensityRange.lMin      = -100;
    si->IntensityRange.lMax      =  100;
    si->IntensityRange.lStep     = 1;
    si->ContrastRange.lMin       = -100;
    si->ContrastRange.lMax       =  100;
    si->ContrastRange.lStep      = 1;

    si->SupportedCompressionType = 0;
    si->SupportedDataTypes       = SUPPORT_BW | SUPPORT_GRAYSCALE | SUPPORT_COLOR;

    si->DataType                 = WIA_DATA_COLOR;
    si->Intensity                = 0;
    si->Contrast                 = 0;
    si->Xresolution              = 300;
    si->Yresolution              = 300;
    si->Window.xPos              = 0;
    si->Window.yPos              = 0;
    si->Window.xExtent           = (si->Xresolution * si->BedWidth) / 1000;
    si->Window.yExtent           = (si->Yresolution * si->BedHeight) / 1000;

    si->DitherPattern            = 0;
    si->Negative                 = 0;
    si->Mirror                   = 0;
    si->AutoBack                 = 0;
    si->ColorDitherPattern       = 0;
    si->ToneMap                  = 0;
    si->Compression              = 0;
    si->RawDataFormat            = WIA_PACKED_PIXEL;
    si->RawPixelOrder            = WIA_ORDER_RGB;
    si->bNeedDataAlignment       = TRUE;   /* wiafbdrv DWORD-aligns lines */
    si->DelayBetweenRead         = 0;
    si->MaxBufferSize            = 256 * 1024;

    UpdateImageInfo(si);
}

static void FreeScanBuffers()
{
    free(g_ctx.saneLine); g_ctx.saneLine = NULL;
    free(g_ctx.outLine);  g_ctx.outLine  = NULL;
}

static void StopScan()
{
    if (g_ctx.scanning && g_ctx.sess)
        n670_cancel(g_ctx.sess);
    g_ctx.scanning = false;
    FreeScanBuffers();
}

static void CloseSession()
{
    StopScan();
    if (g_ctx.sess) {
        n670_close(g_ctx.sess);
        g_ctx.sess = NULL;
    }
    n670_exit();
}

static HRESULT EnsureSession()
{
    if (g_ctx.sess)
        return S_OK;
    /* Always open our own overlapped handle on the port. The host's
     * DeviceIOHandles[0] is synchronous, so a stuck transfer on it blocks
     * forever (hangs the WIA service and the calling app). usbscan.sys
     * allows several opens, so this coexists with wiafbdrv's handle. */
    n670_usb_set_port(g_ctx.port[0] ? g_ctx.port : NULL, NULL, kVid, kPid);
    if (n670_open(NULL, &g_ctx.sess, NULL) != 0) {
        n670_log("wia: open failed: %s\n", n670_last_error());
        n670_exit();             /* allow a clean retry next time */
        return HRESULT_FROM_WIN32(ERROR_NOT_READY);
    }
    return S_OK;
}

static HRESULT StartScan(PSCANINFO si)
{
    HRESULT hr = EnsureSession();
    if (FAILED(hr))
        return hr;

    UpdateImageInfo(si);

    N670Params p;
    memset(&p, 0, sizeof(p));
    switch (si->DataType) {
    case WIA_DATA_THRESHOLD: p.mode = N670_MODE_LINEART; break;
    case WIA_DATA_GRAYSCALE: p.mode = N670_MODE_GRAY;    break;
    default:                 p.mode = N670_MODE_COLOR;   break;
    }
    LONG xres = si->Xresolution > 0 ? si->Xresolution : 300;
    LONG yres = si->Yresolution > 0 ? si->Yresolution : xres;
    p.dpi     = (int)xres;
    p.preview    = g_ctx.preview ? 1 : 0;
    p.brightness = (int)si->Intensity;
    p.contrast   = (int)si->Contrast;
    p.x_mm    = si->Window.xPos    * 25.4 / xres;
    p.y_mm    = si->Window.yPos    * 25.4 / yres;
    p.w_mm    = si->Window.xExtent * 25.4 / xres;
    p.h_mm    = si->Window.yExtent * 25.4 / yres;

    n670_log("wia: scan dt=%ld res=%ldx%ld win=(%ld,%ld %ldx%ld) -> %ld bytes/line x %ld\n",
             si->DataType, xres, yres, si->Window.xPos, si->Window.yPos,
             si->Window.xExtent, si->Window.yExtent, si->WidthBytes, si->Lines);

    if (n670_start(g_ctx.sess, &p, &g_ctx.frame) != 0) {
        n670_log("wia: start failed: %s\n", n670_last_error());
        /* the device may have been re-plugged: drop the session */
        CloseSession();
        return HRESULT_FROM_WIN32(ERROR_NOT_READY);
    }

    g_ctx.outBpl      = si->WidthBytes;
    g_ctx.outLines    = si->Lines;
    g_ctx.linesDone   = 0;
    g_ctx.outPos      = 0;
    g_ctx.haveOutLine = false;
    g_ctx.saneEof     = false;
    g_ctx.negative    = si->Negative != 0;
    g_ctx.dataType    = si->DataType;
    g_ctx.saneLine    = (unsigned char *)malloc(g_ctx.frame.bytes_per_line > 0
                                                ? g_ctx.frame.bytes_per_line : 1);
    g_ctx.outLine     = (unsigned char *)malloc(g_ctx.outBpl > 0 ? g_ctx.outBpl : 1);
    if (!g_ctx.saneLine || !g_ctx.outLine) {
        StopScan();
        return E_OUTOFMEMORY;
    }
    g_ctx.scanning = true;
    return S_OK;
}

/* Produce the next WIA line into g_ctx.outLine. */
static HRESULT NextOutLine()
{
    const bool lineart = (g_ctx.dataType == WIA_DATA_THRESHOLD);
    const int  sbpl    = g_ctx.frame.bytes_per_line;
    /* SANE lineart is 1 = black; WIA wants 1 = white unless configured */
    const bool invertBits = lineart && g_ctx.whiteIsOne;
    const unsigned char white = lineart ? (g_ctx.whiteIsOne ? 0xFF : 0x00) : 0xFF;

    bool got = false;
    if (!g_ctx.saneEof && sbpl > 0) {
        long n = n670_read(g_ctx.sess, g_ctx.saneLine, sbpl);
        if (n < 0)
            return E_FAIL;
        if (n < sbpl) {
            g_ctx.saneEof = true;
            if (n > 0)   /* partial last line: pad it */
                memset(g_ctx.saneLine + n, lineart ? 0x00 : 0xFF, (size_t)(sbpl - n));
        }
        got = (n > 0);
    }

    memset(g_ctx.outLine, white, (size_t)g_ctx.outBpl);
    if (got) {
        LONG copy = g_ctx.outBpl < sbpl ? g_ctx.outBpl : sbpl;
        if (invertBits) {
            for (LONG i = 0; i < copy; i++)
                g_ctx.outLine[i] = (unsigned char)~g_ctx.saneLine[i];
        } else {
            memcpy(g_ctx.outLine, g_ctx.saneLine, (size_t)copy);
        }
    }
    if (g_ctx.negative)
        for (LONG i = 0; i < g_ctx.outBpl; i++)
            g_ctx.outLine[i] = (unsigned char)~g_ctx.outLine[i];

    g_ctx.haveOutLine = true;
    g_ctx.outPos = 0;
    return S_OK;
}

static HRESULT FillBuffer(PBYTE buf, LONG len, LONG *received)
{
    LONG done = 0;
    while (done < len && g_ctx.linesDone < g_ctx.outLines) {
        if (!g_ctx.haveOutLine) {
            HRESULT hr = NextOutLine();
            if (FAILED(hr)) {
                *received = done;
                return hr;
            }
        }
        LONG left = g_ctx.outBpl - g_ctx.outPos;
        LONG n = (len - done) < left ? (len - done) : left;
        memcpy(buf + done, g_ctx.outLine + g_ctx.outPos, (size_t)n);
        done          += n;
        g_ctx.outPos  += n;
        if (g_ctx.outPos >= g_ctx.outBpl) {
            g_ctx.haveOutLine = false;
            g_ctx.linesDone++;
        }
    }
    *received = done;
    return S_OK;
}

/* ------------------------------------------------------------------------ */
/* microdriver exports                                                       */
/* ------------------------------------------------------------------------ */
WIAMICRO_API HRESULT MicroEntry(LONG lCommand, _Inout_ PVAL pValue)
{
    if (!pValue || !pValue->pScanInfo)
        return E_INVALIDARG;

    PSCANINFO si = pValue->pScanInfo;
    HRESULT hr = E_NOTIMPL;

    EnterCriticalSection(&g_ctx.cs);
    switch (lCommand) {
    case CMD_SETSTIDEVICEHKEY:
        if (pValue->pHandle)
            ReadRegistry((HKEY)*pValue->pHandle);
        hr = S_OK;
        break;

    case CMD_INITIALIZE:
        strncpy_s(g_ctx.port, sizeof(g_ctx.port), pValue->szVal, _TRUNCATE);
        g_ctx.borrowed = si->DeviceIOHandles[0];
        if (g_ctx.borrowed == INVALID_HANDLE_VALUE)
            g_ctx.borrowed = NULL;
        SetupLogging();
        n670_log("wia: CMD_INITIALIZE port='%s' handle0=%p\n",
                 g_ctx.port, g_ctx.borrowed);
        InitDefaults(si);
        hr = S_OK;
        break;

    case CMD_UNINITIALIZE:
        n670_log("wia: CMD_UNINITIALIZE\n");
        CloseSession();
        hr = S_OK;
        break;

    case CMD_RESETSCANNER:
    case CMD_STI_DEVICERESET:
    case CMD_STI_DIAGNOSTIC:
        StopScan();
        hr = S_OK;
        break;

    case CMD_STI_GETSTATUS:
        pValue->lVal  = MCRO_STATUS_OK;
        pValue->pGuid = (GUID *)&GUID_NULL;
        hr = S_OK;
        break;

    case CMD_SETXRESOLUTION:
        si->Xresolution = pValue->lVal;
        UpdateImageInfo(si);
        hr = S_OK;
        break;
    case CMD_SETYRESOLUTION:
        si->Yresolution = pValue->lVal;
        UpdateImageInfo(si);
        hr = S_OK;
        break;
    case CMD_SETCONTRAST:
        si->Contrast = pValue->lVal;
        hr = S_OK;
        break;
    case CMD_SETINTENSITY:
        si->Intensity = pValue->lVal;
        hr = S_OK;
        break;
    case CMD_SETDATATYPE:
        si->DataType = pValue->lVal;
        UpdateImageInfo(si);
        hr = S_OK;
        break;
    case CMD_SETNEGATIVE:
        si->Negative = pValue->lVal;
        hr = S_OK;
        break;
    case CMD_SETLAMP:
        hr = S_OK;
        break;

    case CMD_SETSCANMODE:
        if (pValue->lVal == SCANMODE_PREVIEWSCAN) { g_ctx.preview = true;  hr = S_OK; }
        else if (pValue->lVal == SCANMODE_FINALSCAN) { g_ctx.preview = false; hr = S_OK; }
        else hr = E_INVALIDARG;
        break;

    case CMD_GETCAPABILITIES:
        pValue->lVal          = 0;     /* no button events (yet) */
        pValue->pGuid         = NULL;
        pValue->ppButtonNames = NULL;
        hr = S_OK;
        break;

    case CMD_GETADFAVAILABLE:
    case CMD_GETTPAAVAILABLE:
        pValue->lVal = 0;
        hr = S_OK;
        break;

    default:
        /* E_NOTIMPL tells wiafbdrv to use its own default behaviour */
        break;
    }
    LeaveCriticalSection(&g_ctx.cs);
    return hr;
}

WIAMICRO_API HRESULT Scan(_Inout_ PSCANINFO pScanInfo, LONG lPhase,
                          _Out_writes_bytes_(lLength) PBYTE pBuffer,
                          LONG lLength, _Out_ LONG *plReceived)
{
    if (!pScanInfo)
        return E_INVALIDARG;
    if (plReceived)
        *plReceived = 0;

    HRESULT hr = S_OK;
    EnterCriticalSection(&g_ctx.cs);
    switch (lPhase) {
    case SCAN_FIRST:
        StopScan();                       /* in case a previous scan leaked */
        hr = StartScan(pScanInfo);
        if (FAILED(hr))
            break;
        /* fall through: SCAN_FIRST also expects data */
    case SCAN_NEXT:
        if (!g_ctx.scanning || !pBuffer || !plReceived) {
            hr = g_ctx.scanning ? E_INVALIDARG : E_FAIL;
            break;
        }
        hr = FillBuffer(pBuffer, lLength, plReceived);
        if (FAILED(hr))
            n670_log("wia: read failed after %ld/%ld lines: %s\n",
                     g_ctx.linesDone, g_ctx.outLines, n670_last_error());
        break;

    case SCAN_FINISHED:
    default:
        n670_log("wia: SCAN_FINISHED (%ld/%ld lines)\n",
                 g_ctx.linesDone, g_ctx.outLines);
        StopScan();
        hr = S_OK;
        break;
    }
    LeaveCriticalSection(&g_ctx.cs);
    return hr;
}

WIAMICRO_API HRESULT SetPixelWindow(_Inout_ PSCANINFO pScanInfo, LONG x, LONG y,
                                    LONG xExtent, LONG yExtent)
{
    if (!pScanInfo)
        return E_INVALIDARG;
    pScanInfo->Window.xPos    = x;
    pScanInfo->Window.yPos    = y;
    pScanInfo->Window.xExtent = xExtent;
    pScanInfo->Window.yExtent = yExtent;
    UpdateImageInfo(pScanInfo);
    return S_OK;
}
