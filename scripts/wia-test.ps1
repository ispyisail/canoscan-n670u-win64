<#
.SYNOPSIS
  Scan through WIA (the same path Windows Scan / Paint / Word use).
.EXAMPLE
  .\scripts\wia-test.ps1 -Dpi 150 -Intent Color -Out scans\wia.bmp
#>
param(
    [int]$Dpi = 150,
    [ValidateSet('Color', 'Gray', 'Text')][string]$Intent = 'Color',
    [double]$WidthIn = 0,    # 0 = full bed
    [double]$HeightIn = 0,
    [string]$Out = "$PSScriptRoot\..\scans\wia_test.bmp"
)
$ErrorActionPreference = 'Stop'
$WIA_FORMAT_BMP = '{B96B3CAB-0728-11D3-9D7B-0000F81EF32E}'

function Set-WiaProp($props, [int]$id, $value) {
    foreach ($p in $props) { if ($p.PropertyID -eq $id) { $p.Value = $value; return } }
    Write-Warning "property $id not found"
}

$dm   = New-Object -ComObject WIA.DeviceManager
$info = $dm.DeviceInfos | Where-Object { $_.Type -eq 1 } |   # 1 = scanner
        Where-Object { $_.Properties.Item('Name').Value -like '*N670U*' } | Select-Object -First 1
if (-not $info) {
    Write-Host 'WIA scanners seen:'
    $dm.DeviceInfos | ForEach-Object { '  ' + $_.Properties.Item('Name').Value }
    throw 'N670U not found in WIA'
}
Write-Host "WIA device: $($info.Properties.Item('Name').Value)  [$($info.DeviceID)]"
$dev  = $info.Connect()
$item = $dev.Items.Item(1)

Write-Host 'Item properties:'
foreach ($p in $item.Properties) {
    if ($p.PropertyID -in 4103, 4104, 6146, 6147, 6148, 6151, 6152, 6154, 6155) {
        '  {0,-28} {1}' -f $p.Name, $p.Value
    }
}

$intentVal = @{ Color = 1; Gray = 2; Text = 4 }[$Intent]
Set-WiaProp $item.Properties 6146 $intentVal          # WIA_IPS_CUR_INTENT
Set-WiaProp $item.Properties 6147 $Dpi                # X resolution
Set-WiaProp $item.Properties 6148 $Dpi                # Y resolution
Set-WiaProp $item.Properties 6149 0                   # X pos
Set-WiaProp $item.Properties 6150 0                   # Y pos
if ($WidthIn  -gt 0) { Set-WiaProp $item.Properties 6151 ([int]($WidthIn  * $Dpi)) }
if ($HeightIn -gt 0) { Set-WiaProp $item.Properties 6152 ([int]($HeightIn * $Dpi)) }

$sw  = [Diagnostics.Stopwatch]::StartNew()
$img = $item.Transfer($WIA_FORMAT_BMP)
$sw.Stop()
$full = [IO.Path]::GetFullPath($Out)
if (Test-Path $full) { Remove-Item $full }
$img.SaveFile($full)
Write-Host ("Scanned {0}x{1} @ {2} dpi, {3} bpp in {4:N1}s -> {5}" -f `
    $img.Width, $img.Height, $img.HorizontalResolution, $img.PixelDepth, $sw.Elapsed.TotalSeconds, $full)
