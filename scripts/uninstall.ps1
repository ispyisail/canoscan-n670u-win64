<#
.SYNOPSIS
  Remove the N670U driver package and its certificate  (run as Administrator)
#>
#Requires -RunAsAdministrator
param([switch]$KeepCertificate)
$ErrorActionPreference = 'Continue'

# find our package(s) in the driver store by original INF name
$pkgs = (pnputil /enum-drivers) -join "`n" -split "(?=Published Name:)" |
        Where-Object { $_ -match 'Original Name:\s+n670u\.inf' } |
        ForEach-Object { if ($_ -match 'Published Name:\s+(\S+)') { $Matches[1] } }

foreach ($p in $pkgs) {
    Write-Host "Removing $p"
    pnputil /delete-driver $p /uninstall /force
}
if (-not $pkgs) { Write-Host 'No n670u driver package in the driver store.' }

if (-not $KeepCertificate) {
    foreach ($store in 'Root', 'TrustedPublisher') {
        Get-ChildItem "Cert:\LocalMachine\$store" |
            Where-Object Subject -eq 'CN=N670U Local Driver Signing' |
            ForEach-Object { Write-Host "Removing cert from $store"; Remove-Item $_.PSPath }
    }
}
Restart-Service stisvc -ErrorAction SilentlyContinue
