/*
 * usbprobe - diagnose usbscan.sys pipe layout and LM983x register access.
 *   usbprobe            dump pipes + try register reads
 */
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <setupapi.h>
#include <initguid.h>
#include <winioctl.h>
#include <usbscan.h>
#include <stdio.h>
#include <string.h>

#pragma comment(lib, "setupapi.lib")

DEFINE_GUID(GUID_IMG, 0x6bdd1fc6, 0x810f, 0x11d0, 0xbe, 0xc7, 0x08, 0x00, 0x2b, 0xe2, 0x09, 0x2f);

static char g_path[512];

static int find_dev(void)
{
    HDEVINFO di = SetupDiGetClassDevsA(&GUID_IMG, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    SP_DEVICE_INTERFACE_DATA ifd = { sizeof(ifd) };
    DWORD i, need;
    for (i = 0; SetupDiEnumDeviceInterfaces(di, NULL, &GUID_IMG, i, &ifd); i++) {
        char buf[1024];
        PSP_DEVICE_INTERFACE_DETAIL_DATA_A d = (PSP_DEVICE_INTERFACE_DETAIL_DATA_A)buf;
        d->cbSize = sizeof(*d);
        if (SetupDiGetDeviceInterfaceDetailA(di, &ifd, d, sizeof(buf), &need, NULL) &&
            strstr(_strlwr(d->DevicePath), "vid_04a9&pid_220d")) {
            strcpy(g_path, d->DevicePath);
            SetupDiDestroyDeviceInfoList(di);
            return 1;
        }
    }
    SetupDiDestroyDeviceInfoList(di);
    return 0;
}

static HANDLE open_path(const char *suffix)
{
    char p[600];
    snprintf(p, sizeof(p), "%s%s", g_path, suffix ? suffix : "");
    return CreateFileA(p, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL, OPEN_EXISTING, 0, NULL);
}

static void set_to(HANDLE h, ULONG s)
{
    USBSCAN_TIMEOUT to = { s, s, s };
    DWORD r;
    DeviceIoControl(h, IOCTL_SET_TIMEOUT, &to, sizeof(to), NULL, 0, &r, NULL);
}

static const char *ptype(int t)
{
    switch (t) { case 0: return "CONTROL"; case 1: return "ISOCH"; case 2: return "BULK"; case 3: return "INTERRUPT"; }
    return "?";
}

/* LM983x: write {cmd, reg, lenHi, lenLo}; then read len bytes */
static void try_read(const char *label, HANDLE hw, HANDLE hr, BYTE reg, DWORD readlen)
{
    BYTE cmd[4] = { 0x01, reg, 0x00, 0x01 };
    BYTE buf[512];
    DWORD n = 0;
    BOOL ok;
    memset(buf, 0xEE, sizeof(buf));
    ok = WriteFile(hw, cmd, 4, &n, NULL);
    printf("  [%s] reg 0x%02x: write cmd %s (%lu)", label, reg, ok ? "ok" : "FAIL", ok ? n : GetLastError());
    if (!ok) { printf("\n"); return; }
    n = 0;
    ok = ReadFile(hr, buf, readlen, &n, NULL);
    if (ok)
        printf(", read(%lu) -> %lu bytes: %02x %02x\n", readlen, n, buf[0], buf[1]);
    else
        printf(", read(%lu) FAIL err %lu\n", readlen, GetLastError());
}

int main(void)
{
    HANDLE h, hp[8];
    USBSCAN_PIPE_CONFIGURATION pc;
    DEVICE_DESCRIPTOR dd;
    DWORD r, i;
    int bulkIn = -1, bulkOut = -1;

    if (!find_dev()) { printf("no 04a9:220d image interface\n"); return 1; }
    printf("device: %s\n", g_path);
    h = open_path(NULL);
    if (h == INVALID_HANDLE_VALUE) { printf("open failed %lu\n", GetLastError()); return 1; }
    set_to(h, 3);

    if (DeviceIoControl(h, IOCTL_GET_DEVICE_DESCRIPTOR, NULL, 0, &dd, sizeof(dd), &r, NULL))
        printf("ids: %04x:%04x bcd %04x\n", dd.usVendorId, dd.usProductId, dd.usBcdDevice);

    memset(&pc, 0, sizeof(pc));
    if (DeviceIoControl(h, IOCTL_GET_PIPE_CONFIGURATION, NULL, 0, &pc, sizeof(pc), &r, NULL)) {
        printf("pipes: %lu\n", pc.NumberOfPipes);
        for (i = 0; i < pc.NumberOfPipes && i < MAX_NUM_PIPES; i++) {
            printf("  pipe %lu: ep 0x%02x %-9s maxpkt %u interval %u\n", i,
                   pc.PipeInfo[i].EndpointAddress, ptype(pc.PipeInfo[i].PipeType),
                   pc.PipeInfo[i].MaximumPacketSize, pc.PipeInfo[i].Interval);
            if (pc.PipeInfo[i].PipeType == 2 && (pc.PipeInfo[i].EndpointAddress & 0x80) && bulkIn < 0) bulkIn = (int)i;
            if (pc.PipeInfo[i].PipeType == 2 && !(pc.PipeInfo[i].EndpointAddress & 0x80) && bulkOut < 0) bulkOut = (int)i;
        }
    } else {
        printf("GET_PIPE_CONFIGURATION failed %lu\n", GetLastError());
    }

    {
        DWORD p = ALL_PIPE;
        DeviceIoControl(h, IOCTL_RESET_PIPE, &p, sizeof(p), NULL, 0, &r, NULL);
    }

    printf("default handle matrix:\n");
    {
        static const DWORD sizes[] = { 1, 64, 65, 128, 512, 4096 };
        static const BYTE regs[] = { 0x00, 0x02, 0x07, 0x02 };
        DWORD si, ri, rep;
        for (si = 0; si < sizeof(sizes) / sizeof(sizes[0]); si++)
            for (ri = 0; ri < sizeof(regs); ri++)
                for (rep = 0; rep < 2; rep++) {
                    char lbl[32];
                    snprintf(lbl, sizeof(lbl), "sz%lu#%lu", sizes[si], rep);
                    try_read(lbl, h, h, regs[ri], sizes[si]);
                }
    }

    if (bulkIn >= 0 && bulkOut >= 0) {
        char s[8];
        HANDLE hin, hout;
        snprintf(s, sizeof(s), "\\%d", bulkIn);  hin  = open_path(s);
        snprintf(s, sizeof(s), "\\%d", bulkOut); hout = open_path(s);
        printf("explicit pipes in=\\%d out=\\%d: %s\n", bulkIn, bulkOut,
               (hin != INVALID_HANDLE_VALUE && hout != INVALID_HANDLE_VALUE) ? "opened" : "open failed");
        if (hin != INVALID_HANDLE_VALUE && hout != INVALID_HANDLE_VALUE) {
            set_to(hin, 3); set_to(hout, 3);
            try_read("pipe,1", hout, hin, 0x00, 1);
            try_read("pipe,64", hout, hin, 0x00, 64);
            try_read("pipe,512", hout, hin, 0x02, 512);
            CloseHandle(hin); CloseHandle(hout);
        }
    }
    (void)hp;
    CloseHandle(h);
    return 0;
}
