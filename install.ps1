$ErrorActionPreference = 'Stop'

# pocket-dial Windows PowerShell One-Line Installer
# install.ps1: Issue #21 resolved.
# Trust root: the SHA-256 below.
# Do NOT pipe this from a remote URL — that defeats the point.

$tag      = 'v1.0.0'
$expected = 'fbb2be731c2f61c42ea8287fa6da2958235851f05b91c61356ac834240d8d3ba'
$url      = "https://github.com/GlomarGadaffi/pocket-dial/releases/download/$tag/pocket-dial-$tag.zip"

$tmp = Join-Path $env:TEMP ([guid]::NewGuid())
New-Item -ItemType Directory $tmp | Out-Null
$zip = Join-Path $tmp 'pd.zip'

Write-Host "Downloading pocket-dial $tag ..."
Invoke-WebRequest $url -OutFile $zip

Write-Host "Verifying SHA-256 ..."
$actual = (Get-FileHash $zip -Algorithm SHA256).Hash
if ($actual -ne $expected) {
    Write-Error "checksum mismatch — refusing to run.`n  expected: $expected`n  actual:   $actual"
    exit 1
}

Write-Host "Checksum OK."
Expand-Archive $zip -DestinationPath $tmp
Set-Location (Join-Path $tmp "pocket-dial-1.0.0")
.\quickstart.bat
