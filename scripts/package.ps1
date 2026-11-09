<#
.SYNOPSIS
  Build, catalog and sign the N670U driver package into .\dist

  Creates (once) a self-signed code-signing certificate in CurrentUser\My,
  generates n670u.cat with Inf2Cat and signs the catalog + DLL with it.
  Does not need admin. Run install.ps1 (as admin) afterwards.
#>
[CmdletBinding()]
param(
    [string]$Config = 'Release',
    [string]$CertSubject = 'CN=N670U Local Driver Signing'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$kit  = "${env:ProgramFiles(x86)}\Windows Kits\10\bin\10.0.26100.0"
$inf2cat  = "$kit\x86\Inf2Cat.exe"
$signtool = "$kit\x64\signtool.exe"
foreach ($t in $inf2cat, $signtool) { if (-not (Test-Path $t)) { throw "missing $t (install the WDK)" } }

# 1. build
cmake -S $root -B "$root\build" -G "Visual Studio 17 2022" -A x64 | Out-Null
cmake --build "$root\build" --config $Config
if ($LASTEXITCODE) { throw 'build failed' }

# 2. stage
$dist = Join-Path $root 'dist'
if (Test-Path $dist) { Remove-Item $dist -Recurse -Force }
New-Item -ItemType Directory $dist | Out-Null
Copy-Item "$root\driver\n670u.inf"               $dist
Copy-Item "$root\build\$Config\n670mdrv.dll"     $dist
Copy-Item "$root\build\$Config\n670scan.exe"     $dist

# 3. certificate (reuse if present)
$cert = Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert |
        Where-Object { $_.Subject -eq $CertSubject -and $_.NotAfter -gt (Get-Date) } |
        Sort-Object NotAfter -Descending | Select-Object -First 1
if (-not $cert) {
    Write-Host "Creating self-signed code-signing certificate '$CertSubject'"
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $CertSubject `
              -CertStoreLocation Cert:\CurrentUser\My -KeyAlgorithm RSA -KeyLength 3072 `
              -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(10)
}
Export-Certificate -Cert $cert -FilePath "$dist\n670u-signing.cer" | Out-Null
Write-Host "Signing cert: $($cert.Thumbprint)"

# 4. sign the DLL first (catalog hashes the signed file)
& $signtool sign /fd SHA256 /sha1 $cert.Thumbprint /s My "$dist\n670mdrv.dll"
if ($LASTEXITCODE) { throw 'signtool (dll) failed' }

# 5. catalog
& $inf2cat /driver:$dist /os:10_X64,10_RS3_X64,10_VB_X64,10_CO_X64,10_NI_X64,10_GE_X64 /uselocaltime
if ($LASTEXITCODE) { throw 'Inf2Cat failed' }
& $signtool sign /fd SHA256 /sha1 $cert.Thumbprint /s My "$dist\n670u.cat"
if ($LASTEXITCODE) { throw 'signtool (cat) failed' }

Write-Host "`nPackage ready in $dist" -ForegroundColor Green
Get-ChildItem $dist | Format-Table Name, Length
