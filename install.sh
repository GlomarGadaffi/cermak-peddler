#!/bin/sh
set -eu

# pocket-dial Linux/macOS One-Line Installer

TAG="v1.0.0"
URL="https://github.com/GlomarGadaffi/pocket-dial/releases/download/${TAG}/pocket-dial-${TAG}.zip"

T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
echo "Downloading pocket-dial ${TAG} ..."
curl -fsSL "$URL" -o "$T/pd.zip"

unzip -q "$T/pd.zip" -d "$T"
cd "$T/pocket-dial-1.0.0"
chmod +x quickstart.sh && ./quickstart.sh
