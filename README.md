# REAPER SWELL Sidebar Patch

Adds a collapsible sidebar to REAPER's file dialogs on Linux, providing quick navigation to:

- **Places** — Home, Desktop, Downloads, Documents, Music, Pictures, Videos, Trash (from XDG user dirs)
- **Bookmarks** — user-added folders, synced with `~/.config/gtk-3.0/bookmarks`
- **Devices** — mounted block devices from `/proc/mounts`

Also adds full file operations to the dialog: new folder, rename, move to trash, copy/cut/paste, open in file manager, and copy path.

Right-clicking a sidebar item lets you add/remove bookmarks, copy the path, or open in Dolphin.

This patch affects the dialogs opened by **Insert Media**, **Save Project As**, **Browse** buttons in REAPER, and any other dialog that calls `BrowseForFiles`, `BrowseForSaveFile`, or `BrowseForDirectory`.

---

## Install (pre-built)

1. Find your REAPER install directory (commonly `~/opt/REAPER` or `/opt/REAPER`)
2. Back up the original: `cp ~/opt/REAPER/libSwell.so ~/opt/REAPER/libSwell.so.bak`
3. Copy the pre-built binary: `cp releases/libSwell.so ~/opt/REAPER/libSwell.so`
4. Restart REAPER

> **Note:** The pre-built binary was built on CachyOS (Arch-based, kernel 7.x). It should work on most modern Linux distros, but if REAPER crashes on launch, build from source instead.

---

## Build from source

**Dependencies:** `gcc`, `make`, `git`, GTK3 development headers

On Debian/Ubuntu:
```bash
sudo apt install build-essential git libgtk-3-dev
```

On Arch/Manjaro/CachyOS:
```bash
sudo pacman -S base-devel git gtk3
```

**Build and install:**
```bash
./build.sh
# or if your REAPER is in a non-default location:
./build.sh /path/to/REAPER
```

The script clones WDL, applies the patches, builds, and copies `libSwell.so` into your REAPER directory.

---

## After a REAPER update

REAPER updates sometimes ship a new `libSwell.so`, which will overwrite the patch. Just re-run the install:

- **Pre-built**: repeat step 3 above
- **From source**: re-run `./build.sh`

If the patch fails to apply after a REAPER update, open an issue — it likely means Cockos updated the SWELL dialog code and the patch needs a refresh.

---

## How it works

REAPER on Linux uses [SWELL](https://github.com/justinfrankel/WDL/tree/main/WDL/swell) (Simple Windows Emulation Layer Library), part of the [WDL](https://github.com/justinfrankel/WDL) library, to implement Win32-style UI using GTK. REAPER loads `libSwell.so` at startup from its own directory.

This patch modifies two files in SWELL:

- `swell-miscdlg-generic.cpp` — adds the sidebar, bookmark management, and file operations to `swellFileSelectProc`
- `swell-wnd-generic.cpp` — fixes treeview right-click hit-testing and section header rendering

The stock SWELL file dialog has been stable for a long time with no planned changes upstream, so this patch should remain compatible across REAPER updates without needing rebuilds.
