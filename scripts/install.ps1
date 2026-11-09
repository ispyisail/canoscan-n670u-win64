<#
.SYNOPSIS
  Install the N670U driver package from .\dist  (run as Administrator)

  1. Trusts the package's self-signed certificate on THIS machine only
     (LocalMachine\Root + LocalMachine\TrustedPublisher).
  2. Adds the driver to the driver store and installs it on the scanner.
#>
#Requires -RunAsAdministrator
param([int]$DebugLevel = -1)   # >=0: set DeviceData\DebugLevel for the WIA log
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$dist = Join-Path $root 'dist'
$cer  = Join-Path $dist 'n670u-signing.cer'
$inf  = Join-Path $dist 'n670u.inf'
foreach ($f in $cer, $inf, (Join-Path $dist 'n670u.cat')) {
    if (-not (Test-Path $f)) { throw "missing $f - run scripts\package.ps1 first" }
}

Write-Host 'Trusting signing certificate (Root + TrustedPublisher)...'
Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\Root            | Out-Null
Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null

Write-Host 'Creating C:\ProgramData\N670U (log + calibration cache)...'
$data = Join-Path $env:ProgramData 'N670U'
New-Item -ItemType Directory -Force $data | Out-Null
# WIA service runs as LocalService: modify; normal users: read
icacls $data /grant '*S-1-5-19:(OI)(CI)M' '*S-1-5-32-545:(OI)(CI)RX' | Out-Null

Write-Host 'Adding and installing driver...'
pnputil /add-driver $inf /install
$rc = $LASTEXITCODE

# re-evaluate the device so it picks up the new driver right away
$dev = Get-PnpDevice -PresentOnly | Where-Object InstanceId -like 'USB\VID_04A9&PID_220D*'
if ($dev) {
    pnputil /scan-devices | Out-Null
    Start-Sleep -Seconds 2
    Get-PnpDevice -InstanceId $dev.InstanceId | Format-List FriendlyName, Status, Class, Problem
} else {
    Write-Warning 'Scanner not plugged in - it will use the driver when connected.'
}
if ($dev -and $DebugLevel -ge 0) {
    $drvKey = (Get-PnpDeviceProperty -InstanceId $dev.InstanceId -KeyName DEVPKEY_Device_Driver).Data
    $dd = "HKLM:\SYSTEM\CurrentControlSet\Control\Class\$drvKey\DeviceData"
    Set-ItemProperty -Path $dd -Name DebugLevel -Value $DebugLevel -Type DWord
    Write-Host "DebugLevel=$DebugLevel set at $dd"
}
# restart WIA so it loads the microdriver
Restart-Service stisvc -ErrorAction SilentlyContinue
exit $rc
