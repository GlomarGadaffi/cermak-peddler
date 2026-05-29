#!/bin/sh
set -eu

# pocket-dial Linux/macOS One-Line Installer
# install.sh: Issue #21 resolved.
# Trust root: the SHA-256 below.
# The release asset is hashed BEFORE any code from it runs. Mismatch aborts.
# Do NOT pipe this from a remote URL — that defeats the point.
# Read it, then run it locally, or use the one-liner in the README.

TAG="v1.0.0"
EXPECTED_SHA256="423411c556378ab0725011a16df3d3fc8bb6f798af3bdc8263e48aee77ec3f5c"
URL="https://github.com/GlomarGadaffi/pocket-dial/releases/download/${TAG}/pocket-dial-${TAG}.zip"

T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
echo "Downloading pocket-dial ${TAG} ..."
curl -fsSL "$URL" -o "$T/pd.zip"

echo "Verifying SHA-256 ..."
if command -v sha256sum >/dev/null 2>&1; then
    ACTUAL="$(sha256sum "$T/pd.zip" | cut -d' ' -f1)"
elif command -v shasum >/dev/null 2>&1; then
    ACTUAL="$(shasum -a 256 "$T/pd.zip" | cut -d' ' -f1)"
else
    echo "ERROR: no sha256sum/shasum available; cannot verify." >&2; exit 1
fi

if [ "$ACTUAL" != "$EXPECTED_SHA256" ]; then
    echo "ERROR: checksum mismatch — refusing to run." >&2
    echo "  expected: $EXPECTED_SHA256" >&2
    echo "  actual:   $ACTUAL"          >&2
    exit 1
fi

echo "Checksum OK."
unzip -q "$T/pd.zip" -d "$T"
cd "$T/pocket-dial-1.0.0"
chmod +x quickstart.sh && ./quickstart.sh
