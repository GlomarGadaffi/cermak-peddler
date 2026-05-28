#!/bin/bash
set -e

echo "==================================================="
echo "            pocket-dial - Quickstart Build"
echo "==================================================="
echo

# Check if CMake is installed
if ! command -v cmake &> /dev/null; then
    echo "[ERROR] CMake was not found."
    echo "Please install CMake using your package manager (e.g. apt, brew, pacman):"
    echo "  Ubuntu/Debian: sudo apt install cmake build-essential"
    echo "  macOS:         brew install cmake"
    echo
    exit 1
fi

echo "[1/3] Configuring project with CMake..."
cmake -B build -DCMAKE_BUILD_TYPE=Release

echo
echo "[2/3] Building executable..."
cmake --build build --config Release

echo
echo "[3/3] Launching pocket-dial server..."
echo

# Detect binary location
if [ -f "build/Release/SipServer" ]; then
    RUN_CMD="build/Release/SipServer"
elif [ -f "build/SipServer" ]; then
    RUN_CMD="build/SipServer"
else
    echo "[ERROR] Could not locate the built SipServer binary."
    exit 1
fi

# Start the web browser automatically based on OS
echo "Starting dashboard in your default browser..."
if command -v xdg-open &> /dev/null; then
    xdg-open "http://localhost:8080/" &
elif command -v open &> /dev/null; then
    open "http://localhost:8080/" &
fi

# Run the server
exec "$RUN_CMD"
