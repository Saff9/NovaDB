#!/bin/sh
# NovaDB — One-command setup
# curl -fsSL https://raw.githubusercontent.com/novadb/novadb/main/setup.sh | sh
set -e

echo ""
echo "  ========================================"
echo "   NovaDB Setup — Open-Source SQL Engine"
echo "   github.com/Saff9/NovaDB"
echo "  ========================================"
echo ""

# Check for C compiler
if command -v cc >/dev/null 2>&1; then
    echo "  [OK] Found compiler: $(cc --version 2>&1 | head -1)"
elif command -v gcc >/dev/null 2>&1; then
    echo "  [OK] Found compiler: $(gcc --version 2>&1 | head -1)"
elif command -v clang >/dev/null 2>&1; then
    echo "  [OK] Found compiler: $(clang --version 2>&1 | head -1)"
else
    echo "  [!!] No C compiler found."
    echo ""
    echo "  How to get one:"
    echo "    Ubuntu/Debian:  sudo apt install build-essential"
    echo "    Fedora:         sudo dnf install gcc make"
    echo "    Arch:           sudo pacman -S gcc make"
    echo "    macOS:          xcode-select --install"
    exit 1
fi

echo "  [OK] Platform: $(uname -s) $(uname -m)"
echo ""

# Build
echo "  Building..."
make release 2>&1 | tail -3
echo ""
echo "  ========================================"
echo "   Build successful!"
echo "  ========================================"
echo ""

# Test
echo "  Running tests..."
make test 2>&1 | tail -5
echo ""

# Done
echo "  ========================================"
echo "   Ready. Start the server:"
echo ""
echo "     ./bin/novadb-server --data-dir ./mydata --port 9876"
echo ""
echo "   Connect with the CLI:"
echo "     ./bin/novadb-cli"
echo ""
echo "   Or use netcat:"
echo "     echo -e 'PING 0\r\n' | nc localhost 9876"
echo "  ========================================"
echo ""

# Install system-wide if requested
if [ "$1" = "--install" ]; then
    if [ "$(id -u)" -ne 0 ]; then
        echo "  Run with sudo to install: sudo ./setup.sh --install"
    else
        cp bin/novadb-server /usr/local/bin/
        cp bin/novadb-cli /usr/local/bin/
        mkdir -p /var/lib/novadb
        echo "  Installed to /usr/local/bin"
    fi
fi
