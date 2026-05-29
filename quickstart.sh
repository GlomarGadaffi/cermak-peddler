#!/bin/bash
set -e

# Check if --service flag is present
INSTALL_SERVICE=false
for arg in "$@"; do
    if [ "$arg" = "--service" ]; then
        INSTALL_SERVICE=true
    fi
done

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

# Detect binary location
if [ -f "build/Release/SipServer" ]; then
    RUN_CMD="build/Release/SipServer"
elif [ -f "build/SipServer" ]; then
    RUN_CMD="build/SipServer"
else
    echo "[ERROR] Could not locate the built SipServer binary."
    exit 1
fi

if [ "$INSTALL_SERVICE" = true ]; then
    echo "[3/3] Installing pocket-dial as a system service..."
    echo

    # Check if systemctl/systemd is available
    if ! command -v systemctl &> /dev/null; then
        echo "[ERROR] systemd was not found on this system. Cannot install as a service."
        exit 1
    fi

    echo "Copying binary to /usr/local/bin/SipServer..."
    sudo cp "$RUN_CMD" /usr/local/bin/SipServer
    sudo chmod +x /usr/local/bin/SipServer

    echo "Creating systemd service file..."
    sudo tee /etc/systemd/system/pocket-dial.service > /dev/null <<EOF
[Unit]
Description=Pocket-Dial SIP Server
After=network.target

[Service]
Type=simple
ExecStart=/usr/local/bin/SipServer --ip 0.0.0.0 --port 5060 --web 8080
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

    echo "Reloading systemd daemon..."
    sudo systemctl daemon-reload

    echo "Registering/enabling service (not starting)..."
    sudo systemctl enable pocket-dial

    echo
    echo "==================================================="
    echo " [SUCCESS] pocket-dial installed as a system service!"
    echo "==================================================="
    echo "The service has been enabled but is NOT currently running."
    echo "You can manage the service using the following commands:"
    echo "  Start:   sudo systemctl start pocket-dial"
    echo "  Stop:    sudo systemctl stop pocket-dial"
    echo "  Status:  sudo systemctl status pocket-dial"
    echo "  Logs:    sudo journalctl -u pocket-dial -f"
    echo "==================================================="
    echo
    exit 0
fi

echo "[3/3] Launching pocket-dial server..."
echo

# Start the web browser automatically based on OS
echo "Starting dashboard in your default browser..."
if command -v xdg-open &> /dev/null; then
    xdg-open "http://localhost:8080/" &
elif command -v open &> /dev/null; then
    open "http://localhost:8080/" &
fi

# Run the server
exec "$RUN_CMD"
