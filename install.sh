#!/usr/bin/env bash
set -e

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WDL_DIR="/tmp/WDL-swell-build"

# --- find REAPER ---
find_reaper() {
    for candidate in "$HOME/opt/REAPER" "/opt/REAPER"; do
        if [[ -f "$candidate/reaper" ]]; then
            echo "$candidate"
            return
        fi
    done
    # search common locations
    local found
    found=$(find "$HOME" /opt -maxdepth 3 -name "reaper" -type f 2>/dev/null | head -1)
    if [[ -n "$found" ]]; then
        dirname "$found"
    fi
}

REAPER_DIR="${REAPER_DIR:-$(find_reaper)}"

if [[ -z "$REAPER_DIR" ]]; then
    echo "Could not find REAPER installation."
    read -rp "Enter full path to your REAPER directory (e.g. /home/you/opt/REAPER): " REAPER_DIR
fi

if [[ ! -f "$REAPER_DIR/reaper" ]]; then
    echo "Error: no 'reaper' binary found at $REAPER_DIR"
    exit 1
fi

echo "REAPER found at: $REAPER_DIR"

# --- install build deps ---
install_deps() {
    if command -v apt-get &>/dev/null; then
        echo "Detected apt — installing build deps..."
        sudo apt-get install -y build-essential libgtk-3-dev git
    elif command -v dnf &>/dev/null; then
        echo "Detected dnf — installing build deps..."
        sudo dnf install -y gcc gcc-c++ make gtk3-devel git
    elif command -v pacman &>/dev/null; then
        echo "Detected pacman — installing build deps..."
        sudo pacman -S --needed --noconfirm base-devel gtk3 git
    elif command -v zypper &>/dev/null; then
        echo "Detected zypper — installing build deps..."
        sudo zypper install -y gcc make gtk3-devel git
    else
        echo "Unknown package manager. Please install manually: gcc, make, GTK3 dev headers, git"
        read -rp "Press Enter when ready, or Ctrl+C to abort."
    fi
}

install_deps

# --- clone WDL ---
echo "Cloning WDL..."
rm -rf "$WDL_DIR"
git clone --depth=1 https://github.com/justinfrankel/WDL.git "$WDL_DIR"

# --- copy patched sources ---
echo "Applying patches..."
cp "$REPO_DIR/src/swell-miscdlg-generic.cpp" "$WDL_DIR/WDL/swell/"
cp "$REPO_DIR/src/swell-wnd-generic.cpp"     "$WDL_DIR/WDL/swell/"

# --- build ---
echo "Building libSwell.so..."
make -C "$WDL_DIR/WDL/swell" PRELOAD_GDK=1 -j"$(nproc)"

# --- install ---
DEST="$REAPER_DIR/libSwell.so"
if [[ ! -w "$DEST" ]] && [[ -f "$DEST" ]]; then
    echo "libSwell.so at $DEST is not writable — trying with sudo..."
    sudo cp "$WDL_DIR/WDL/swell/libSwell.so" "$DEST"
else
    cp "$WDL_DIR/WDL/swell/libSwell.so" "$DEST"
fi

echo ""
echo "Done. libSwell.so installed to $DEST"
echo "Start REAPER to verify."
