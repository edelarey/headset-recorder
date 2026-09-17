#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"

if ! command -v gcc >/dev/null 2>&1 || ! pkg-config --exists gtk+-3.0; then
  echo "Need gcc and GTK 3 headers. Install with:"
  echo "  sudo apt install build-essential pkg-config libgtk-3-dev"
  exit 1
fi

gcc -O2 -Wall -o headset-recorder main.c \
  $(pkg-config --cflags --libs gtk+-3.0) -pthread -lm

echo "Built ./headset-recorder"
echo "Run:  ./headset-recorder"
echo "Install to the desktop dock:  ./install.sh"
