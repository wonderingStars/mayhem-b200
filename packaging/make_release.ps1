# mayhem-b200 — assemble a self-contained Windows release ZIP.
#
# Bundles the exe with the UHD + libusb + VC++ runtime DLLs and the B200/B210
# firmware/FPGA images, so an end user needs no separate UHD/Boost/MSVC install
# (only the one-time Zadig WinUSB driver step, documented in READ-ME-FIRST.txt).
#
# Usage (from the repo root, after building the Release exe):
#   powershell -ExecutionPolicy Bypass -File packaging\make_release.ps1
#
# SPDX-License-Identifier: GPL-2.0-or-later

$ErrorActionPreference = "Stop"

$repo    = Split-Path -Parent $PSScriptRoot
$version = (Select-String -Path "$repo\src\apps\about_app.hpp" -Pattern 'kVersion\s*=\s*"([^"]+)"').Matches[0].Groups[1].Value
$name    = "mayhem-b200-$version-win64"
$stage   = Join-Path $repo "dist\$name"
$uhd     = "C:\Program Files\UHD"
$crt     = Get-ChildItem "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Redist\MSVC\*\x64\Microsoft.VC143.CRT" -Directory | Select-Object -Last 1

Write-Host "Packaging $name"

if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force -Path $stage | Out-Null
New-Item -ItemType Directory -Force -Path "$stage\uhd-images" | Out-Null

# --- the application ---
Copy-Item "$repo\build\mayhem-b200.exe" $stage

# --- UHD + libusb runtime ---
Copy-Item "$uhd\bin\uhd.dll" $stage
Copy-Item "$uhd\bin\libusb-1.0.dll" $stage

# --- VC++ runtime (the app and uhd.dll are /MD) ---
foreach ($dll in "msvcp140.dll","msvcp140_1.dll","msvcp140_2.dll","msvcp140_atomic_wait.dll","vcruntime140.dll","vcruntime140_1.dll","concrt140.dll") {
    Copy-Item (Join-Path $crt.FullName $dll) $stage
}

# --- UHD images the B200/B210 need to initialise ---
foreach ($img in "usrp_b200_fw.hex","usrp_b200_fpga.bin","usrp_b210_fpga.bin","usrp_b200mini_fpga.bin","usrp_b205mini_fpga.bin") {
    $src = Join-Path "$uhd\share\uhd\images" $img
    if (Test-Path $src) { Copy-Item $src "$stage\uhd-images" }
}

# --- docs / licensing ---
Copy-Item "$repo\packaging\READ-ME-FIRST.txt" $stage
Copy-Item "$repo\LICENSE" $stage
Copy-Item "$repo\LICENSE.GPL-2.0-or-later" $stage
Copy-Item "$repo\NOTICE.md" $stage

# --- zip it ---
$zip = Join-Path $repo "dist\$name.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path "$stage\*" -DestinationPath $zip -CompressionLevel Optimal

$mb = [math]::Round((Get-Item $zip).Length / 1MB, 1)
Write-Host "`nBuilt $zip ($mb MB)"
Write-Host "Contents:"
Get-ChildItem -Recurse $stage | ForEach-Object { "  " + $_.FullName.Substring($stage.Length + 1) } | Where-Object { $_ -notmatch '\\$' }
