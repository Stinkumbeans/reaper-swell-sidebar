# REAPER SWELL Sidebar Patch

Adds a collapsible sidebar to REAPER's stock file dialogs (open, save, browse for directory) on Linux.

**Sidebar sections:**
- **Places** — Home, Desktop, Downloads, Documents, Music, Pictures, Videos, Trash (from XDG user dirs)
- **Bookmarks** — your GTK bookmarks (`~/.config/gtk-3.0/bookmarks`), with right-click add/remove
- **Devices** — mounted block devices from `/proc/mounts`

**File operations** (right-click in file list or keyboard shortcuts):
- New folder (`Ctrl+Shift+N`), Rename (`F2`), Move to Trash (`Del`)
- Copy / Cut / Paste (`Ctrl+C` / `X` / `V`), Copy path (`Ctrl+Shift+C`)
- Open in file manager

> This patch only affects the three stock SWELL dialogs (`BrowseForFiles`, `BrowseForSaveFile`, `BrowseForDirectory`). REAPER's Insert Media dialog is separate and unaffected.

---

## Requirements

- Linux, GTK3
- REAPER installed (tested with REAPER 7.x)
- `git`, `gcc`/`g++`, `make`, GTK3 dev headers — the install script handles these

---

## Install

```bash
git clone https://github.com/Stinkumbeans/reaper-swell-sidebar.git
cd reaper-swell-sidebar
chmod +x install.sh
./install.sh
```

The script will:
1. Detect your package manager and install build dependencies
2. Clone the WDL library from source
3. Drop in the patched files and build `libSwell.so`
4. Install it alongside your REAPER binary

If REAPER is in a non-standard location, set `REAPER_DIR` first:

```bash
REAPER_DIR=/path/to/REAPER ./install.sh
```

### Tested on

| Distro | Status |
|---|---|
| CachyOS (Arch-based) | Working |
| Linux Mint (Debian/Ubuntu-based) | Build from source required (use this script) |

---

## After a REAPER update

REAPER updates will overwrite `libSwell.so`. Re-run `./install.sh` to restore the patch.

---

## Uninstall

Delete `libSwell.so` from your REAPER directory. REAPER will fall back to its built-in version on next launch.

```bash
rm ~/opt/REAPER/libSwell.so   # adjust path if needed
```
