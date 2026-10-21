#!/usr/bin/env bash
set -e

# WDL commit this patch was developed and tested against
WDL_COMMIT="61c5553"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="/tmp/WDL-swell-sidebar-build"
REAPER_DIR="${1:-$HOME/opt/REAPER}"

if [ ! -d "$REAPER_DIR" ]; then
    echo "REAPER directory not found at $REAPER_DIR"
    echo "Usage: $0 [/path/to/REAPER]"
    exit 1
fi

echo "Cloning WDL..."
rm -rf "$BUILD_DIR"
git clone --depth=1 https://github.com/justinfrankel/WDL.git "$BUILD_DIR"

echo "Applying patches..."
patch "$BUILD_DIR/WDL/swell/swell-miscdlg-generic.cpp" < "$SCRIPT_DIR/patch/swell-miscdlg-generic.diff"
patch "$BUILD_DIR/WDL/swell/swell-wnd-generic.cpp" < "$SCRIPT_DIR/patch/swell-wnd-generic.diff"

echo "Building..."
make -C "$BUILD_DIR/WDL/swell" PRELOAD_GDK=1

echo "Installing to $REAPER_DIR..."
cp "$BUILD_DIR/WDL/swell/libSwell.so" "$REAPER_DIR/libSwell.so"

echo "Done. Restart REAPER to apply."
