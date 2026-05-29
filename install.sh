#!/bin/sh
set -eu

# default to latest stable release tag
TAG="v1.1.0"
ZIP_DIR="pocket-dial-1.1.0"
URL="https://github.com/GlomarGadaffi/pocket-dial/releases/download/${TAG}/pocket-dial-${TAG}.zip"

# check if we requested bleeding edge / unreleased
REQ_BRANCH="${1:-${POCKET_DIAL_BRANCH:-}}"
if [ "$REQ_BRANCH" = "main" ] || [ "$REQ_BRANCH" = "unreleased" ]; then
    echo "Using bleeding-edge (unreleased) main branch..."
    URL="https://github.com/GlomarGadaffi/pocket-dial/archive/refs/heads/main.zip"
    ZIP_DIR="pocket-dial-main"
fi

T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
echo "Downloading pocket-dial..."
curl -fsSL "$URL" -o "$T/pd.zip"

unzip -q "$T/pd.zip" -d "$T"
cd "$T/$ZIP_DIR"
chmod +x quickstart.sh && ./quickstart.sh
