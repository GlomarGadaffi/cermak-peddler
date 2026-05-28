# pocket-dial Windows PowerShell One-Line Installer
# Pinned to the v1.0.0 release tag so this script always builds a known-good
# version and is not affected by subsequent commits to main.
$ReleaseTag = "v1.0.0"
$ArchiveDir = "pocket-dial-1.0.0"

Write-Host "===================================================" -ForegroundColor Cyan
Write-Host "          pocket-dial Installer Pipeline" -ForegroundColor Cyan
Write-Host "          Release: $ReleaseTag" -ForegroundColor Cyan
Write-Host "===================================================" -ForegroundColor Cyan
Write-Host "Downloading pocket-dial $ReleaseTag from GitHub..."
Invoke-WebRequest -Uri "https://github.com/GlomarGadaffi/pocket-dial/archive/refs/tags/$ReleaseTag.zip" -OutFile "pd_temp.zip"

Write-Host "Extracting repository archive..."
tar -xf pd_temp.zip
Remove-Item "pd_temp.zip" -Force

Write-Host "Entering $ArchiveDir directory..."
cd $ArchiveDir

Write-Host "Executing quickstart build script..."
.\quickstart.bat
