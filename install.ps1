$ErrorActionPreference = 'Stop'

# default to latest stable release tag
$tag     = 'v1.1.0'
$zipDir  = 'pocket-dial-1.1.0'
$url     = "https://github.com/GlomarGadaffi/pocket-dial/archive/refs/tags/$tag.zip"

# check if we requested bleeding edge / unreleased
$reqBranch = $args[0]
if (!$reqBranch) {
    $reqBranch = $env:POCKET_DIAL_BRANCH
}

if ($reqBranch -eq 'main' -or $reqBranch -eq 'unreleased') {
    Write-Host "Using bleeding-edge (unreleased) main branch..."
    $url    = "https://github.com/GlomarGadaffi/pocket-dial/archive/refs/heads/main.zip"
    $zipDir = "pocket-dial-main"
}

$tmp = Join-Path $env:TEMP ([guid]::NewGuid())
New-Item -ItemType Directory $tmp | Out-Null
$zip = Join-Path $tmp 'pd.zip'

Write-Host "Downloading pocket-dial..."
Invoke-WebRequest $url -OutFile $zip

Expand-Archive $zip -DestinationPath $tmp
Set-Location (Join-Path $tmp $zipDir)
.\quickstart.bat
