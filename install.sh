#!/bin/sh
set -eu

# default to latest stable release tag
TAG="v1.1.0"
ZIP_DIR="pocket-dial-1.1.0"
URL="https://github.com/GlomarGadaffi/pocket-dial/archive/refs/tags/${TAG}.zip"

# check if we requested bleeding edge / unreleased
REQ_BRANCH="${1:-${POCKET_DIAL_BRANCH:-}}"
if [ "$REQ_BRANCH" = "main" ] || [ "$REQ_BRANCH" = "unreleased" ]; then
    echo "Using bleeding-edge (unreleased) main branch..."
    URL="https://github.com/GlomarGadaffi/pocket-dial/archive/refs/heads/main.zip"
    ZIP_DIR="pocket-dial-main"
fi

# check if --service flag is present
INSTALL_SERVICE=false
for arg in "$@"; do
    if [ "$arg" = "--service" ]; then
        INSTALL_SERVICE=true
    fi
done

T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
echo "Downloading pocket-dial..."
curl -fsSL "$URL" -o "$T/pd.zip"

unzip -q "$T/pd.zip" -d "$T"
cd "$T/$ZIP_DIR"
chmod +x quickstart.sh

if [ "$INSTALL_SERVICE" = true ]; then
    ./quickstart.sh --service
else
    ./quickstart.sh
fi
