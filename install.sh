#!/bin/bash
set -e

# pocket-dial Linux/macOS One-Line Installer
echo "==================================================="
echo "          pocket-dial Installer Pipeline"
echo "==================================================="
echo "Downloading pocket-dial source from GitHub..."
curl -L https://github.com/GlomarGadaffi/pocket-dial/archive/refs/heads/main.zip -o pd_temp.zip

echo "Extracting repository archive..."
tar -xf pd_temp.zip
rm pd_temp.zip

echo "Entering pocket-dial-main directory..."
cd pocket-dial-main

echo "Configuring permissions and executing quickstart..."
chmod +x quickstart.sh
./quickstart.sh
