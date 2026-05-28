#!/bin/bash
set -e

# pocket-dial Linux/macOS One-Line Installer
# Pinned to the v1.0.0 release tag so this script always builds a known-good
# version and is not affected by subsequent commits to main.
RELEASE_TAG="v1.0.0"
ARCHIVE_DIR="pocket-dial-1.0.0"

echo "==================================================="
echo "          pocket-dial Installer Pipeline"
echo "          Release: ${RELEASE_TAG}"
echo "==================================================="
echo "Downloading pocket-dial ${RELEASE_TAG} from GitHub..."
curl -L "https://github.com/GlomarGadaffi/pocket-dial/archive/refs/tags/${RELEASE_TAG}.zip" -o pd_temp.zip

echo "Extracting repository archive..."
tar -xf pd_temp.zip
rm pd_temp.zip

echo "Entering ${ARCHIVE_DIR} directory..."
cd "${ARCHIVE_DIR}"

echo "Configuring permissions and executing quickstart..."
chmod +x quickstart.sh
./quickstart.sh
