$ErrorActionPreference = 'Stop'

# pocket-dial Windows PowerShell One-Line Installer

$tag      = 'v1.0.0'
$url      = "https://github.com/GlomarGadaffi/pocket-dial/releases/download/$tag/pocket-dial-$tag.zip"

$tmp = Join-Path $env:TEMP ([guid]::NewGuid())
New-Item -ItemType Directory $tmp | Out-Null
$zip = Join-Path $tmp 'pd.zip'

Write-Host "Downloading pocket-dial $tag ..."
Invoke-WebRequest $url -OutFile $zip

Expand-Archive $zip -DestinationPath $tmp
Set-Location (Join-Path $tmp "pocket-dial-1.0.0")
.\quickstart.bat
