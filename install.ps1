# pocket-dial Windows PowerShell One-Line Installer
Write-Host "===================================================" -ForegroundColor Cyan
Write-Host "          pocket-dial Installer Pipeline" -ForegroundColor Cyan
Write-Host "===================================================" -ForegroundColor Cyan
Write-Host "Downloading pocket-dial source from GitHub..."
Invoke-WebRequest -Uri "https://github.com/GlomarGadaffi/pocket-dial/archive/refs/heads/main.zip" -OutFile "pd_temp.zip"

Write-Host "Extracting repository archive..."
tar -xf pd_temp.zip
Remove-Item "pd_temp.zip" -Force

Write-Host "Entering pocket-dial-main directory..."
cd "pocket-dial-main"

Write-Host "Executing quickstart build script..."
.\quickstart.bat
