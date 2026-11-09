# CanoScan N670U / N676U / LiDE20 driver for 64-bit Windows 10/11

An unofficial WIA scanner driver that brings the **Canon CanoScan N670U,
N676U and LiDE20** (USB ID `04A9:220D`) back to life on **64-bit Windows 10
and 11**. Canon only ever shipped 32-bit drivers for these scanners.

Once installed, the scanner works in any normal Windows scanning app:
**Paint → From scanner**, **Windows Fax and Scan**, Word, GIMP (WIA), etc.

> Not affiliated with or endorsed by Canon Inc. Use at your own risk.

## How it works

The Linux [SANE](http://www.sane-project.org/) project has supported these
scanners for years in its `plustek` backend. This project compiles that
backend for Windows x64 and plugs it into Windows' own scanner stack:

```
 Paint / Fax and Scan / Word ...
            │  WIA
            ▼
   wiafbdrv.dll   Microsoft "WIA flatbed" driver (built into Windows)
            │  MicroEntry / Scan / SetPixelWindow
            ▼
   n670mdrv.dll   ← this project: SANE plustek backend as a WIA microdriver
            │  ReadFile / WriteFile (USB bulk pipes)
            ▼
   usbscan.sys    Microsoft's signed USB still-image driver (built into Windows)
```

**No kernel-mode code is installed.** The only driver binary in the kernel
is Microsoft's own signed `usbscan.sys`, so **Secure Boot and driver
signature enforcement stay on** – no test-signing mode needed. The driver
package is signed with a self-signed certificate that the build script
generates on *your* machine and trusts on *your* machine only.

## Status

Tested on Windows 11 Pro x64 (build 26300) with a CanoScan N670U.

| Feature | Status |
|---|---|
| Colour scanning via WIA (Paint) | ✅ working |
| Colour / greyscale via command-line tool | ✅ working |
| Greyscale via WIA | ⚠️ should work, not yet confirmed |
| Black & white (line art) via WIA | ⚠️ untested – may come out inverted (see [Tuning](#tuning)) |
| Resolutions 75 – 600 dpi | ✅ |
| Scanner front-panel buttons | ❌ not supported |
| N676U / LiDE20 | ⚠️ same USB ID and SANE profile as the N670U, untested |

Reports for the ⚠️ items are very welcome – please open an issue.

**Speed:** this is a 2003-era USB 1.1 scanner. A full A4 colour page at
75–150 dpi takes about **30 s** for the first scan (warm-up + calibration)
and about **20 s** afterwards (calibration is cached). Higher resolutions take
proportionally longer.

## Installation

You build the driver yourself – this is what lets the package be signed with
a certificate that exists only on your PC. It takes about 30 minutes, most of
it downloading Microsoft's build tools.

### 1. Install the build tools (one-time)

Open **PowerShell** and run:

```powershell
# Visual Studio 2022 Build Tools: C++ compiler + Windows 11 SDK  (~6-8 GB)
winget install --id Microsoft.VisualStudio.2022.BuildTools --override "--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.Windows11SDK.26100 --includeRecommended"

# Windows Driver Kit (provides Inf2Cat, used to build the driver catalog)
winget install --id Microsoft.WindowsWDK.10.0.26100

# CMake and Git, if you don't have them
winget install --id Kitware.CMake
winget install --id Git.Git
```

Accept the UAC prompts. Open a **new** PowerShell window afterwards so the
new tools are on your PATH.

> If Build Tools are already installed but the install above fails, open
> *Visual Studio Installer → Modify* and tick **Desktop development with C++**
> and **Windows 11 SDK (10.0.26100)**.

### 2. Get the source and build the driver package

```powershell
git clone https://github.com/ispyisail/canoscan-n670u-win64.git
cd canoscan-n670u-win64
powershell -ExecutionPolicy Bypass -File .\scripts\package.ps1
```

This compiles everything, creates a code-signing certificate
(`CN=N670U Local Driver Signing`) in your user certificate store, generates
the driver catalog and signs it. The result is in `.\dist\`.

### 3. Install

Plug in the scanner, then in an **Administrator** PowerShell, in the same
folder:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\install.ps1
```

This:
1. trusts your certificate on this machine (Trusted Root + Trusted Publisher),
2. creates `C:\ProgramData\N670U` (calibration cache / optional log),
3. installs the driver with `pnputil` and restarts the WIA service.

Device Manager should now show **Canon CanoScan N670U/N676U/LiDE20** under
*Imaging devices* with no warning icon.

### 4. Scan

Open **Paint → File → From scanner**, pick the CanoScan, scan. The scanner
lights up, calibrates for ~10 s, then the head moves across the page.

## Uninstall

In an Administrator PowerShell in the project folder:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\uninstall.ps1
```

Removes the driver package(s) and the certificate from the machine stores.
Optionally also delete your private key (see below) and `C:\ProgramData\N670U`.

## Security notes

* The certificate's **private key** stays in your user store
  (`Cert:\CurrentUser\My`). Anything signed with it would be trusted by your
  PC. Once you're happy with the install and don't plan to rebuild, you can
  delete it:
  ```powershell
  Get-ChildItem Cert:\CurrentUser\My | ? Subject -eq 'CN=N670U Local Driver Signing' | Remove-Item
  ```
  (`package.ps1` simply creates a new one next time you build.)
* **Never install a certificate someone else sends you** to make a driver
  install – that's why this project ships source and a build script rather
  than pre-signed binaries.

## Tuning

Settings live in the registry under the scanner's driver key:

```
HKLM\SYSTEM\CurrentControlSet\Control\Class\{6bdd1fc6-810f-11d0-bec7-08002be2092f}\<NNNN>\DeviceData
```

(`<NNNN>` is usually `0000`; Device Manager → scanner → Details → *Driver
key* shows it.) Restart the **Windows Image Acquisition (WIA)** service after
changing anything.

| Value | Default | Meaning |
|---|---|---|
| `Gamma` | `100` | Tone curve in percent. 100 = linear (as SANE). 90–95 = darker midtones / richer colour |
| `LineartWhiteIsOne` | `1` | Set to `0` if black & white scans come out inverted |
| `DebugLevel` | `0` | `10`+ writes a log to `C:\ProgramData\N670U\n670u.log` |

**Calibration cache:** the first scan at each resolution calibrates fully and
saves the result to `C:\ProgramData\N670U\*.cal`. Delete those files to force
a fresh calibration – e.g. after cleaning the white calibration strip under
the lid, or if scans start showing streaks or a colour cast.

## Troubleshooting

* **Very bright areas come out pure white** (e.g. pale sky in photos) – the
  sensor saturates; this is a hardware limit, no setting recovers it.
* **"Scanner is busy"** – only one program can use the scanner at a time
  (the WIA driver and the command-line tool share a lock).
* **Scan hangs or fails** – unplug the scanner for 5 s, plug it back in, try
  again. If it persists, set `DebugLevel` to `12`, restart the WIA service,
  reproduce, and attach `C:\ProgramData\N670U\n670u.log` to an issue.
* **Command-line test without WIA**:
  ```powershell
  .\dist\n670scan.exe --list
  .\dist\n670scan.exe --mode color --res 150 -o test.bmp
  .\dist\n670scan.exe --help        # all options (--gamma, --area, --debug ...)
  ```
* **Scripted WIA test**: `.\scripts\wia-test.ps1 -Dpi 150 -Intent Color`
  (run from a normal PowerShell window).

## Project layout

| Path | What |
|---|---|
| `src/sane/` | SANE `plustek` backend + headers (upstream commit in `UPSTREAM_VERSION`) with small `#ifdef _WIN32` patches |
| `src/win32/` | POSIX shim and Win32 implementations of `sanei_usb` (over usbscan.sys), `sanei_thread`, `sanei_access`, config, debug |
| `src/host/` | small session API over the SANE backend, shared by the tools and the driver |
| `wia/` | `n670mdrv.dll` – the WIA microdriver |
| `tools/` | `n670scan.exe` (command-line scanner), `usbprobe.exe` (USB pipe diagnostics) |
| `driver/n670u.inf` | driver INF (pulls in `usbscan.sys` + `wiafbdrv.dll` via `sti.inf`) |
| `scripts/` | `package.ps1`, `install.ps1`, `uninstall.ps1`, `wia-test.ps1` |

### Windows-specific changes to the SANE code

All guarded by `#ifdef _WIN32` so the upstream code paths are unchanged:

* reader "process" runs as a thread; cancellation via the existing `cancelRead` flag
* calibration cache stored in the driver's data directory instead of `$HOME`
* extra 10 ms settle delays after the CIS MISC-I/O register writes – on
  XHCI (USB 3) controllers the LM9832 otherwise stalls the next command
  (upstream already notes this for the N650U)
* register reads are retried with the command re-sent; bounded retry loops
* calibration caching on by default; gamma configurable

## Credits

* The [SANE project](http://www.sane-project.org/) and the authors of the
  `plustek` backend (Gerhard Jäger et al.), who did the real reverse
  engineering of these scanners.
* Microsoft's WIA microdriver sample (`Windows-driver-samples/wia/microdrv`)
  for documenting the microdriver interface.

## Licence

GPL-2.0-or-later, the licence of the SANE backend this is built from (see
[LICENSE](LICENSE); the SANE linking exception is described in the source
headers). The Windows glue code, tools, scripts and WIA microdriver in this
repository are offered under the same terms.
