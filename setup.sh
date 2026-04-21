#!/usr/bin/env bash
# setup.sh  –  Install build dependencies for IDUN Doom on Arch Linux ARM
# Run once on the IDUN Pi Zero 2 W:  bash setup.sh
set -e

echo "=== IDUN Doom setup ==="

# ── package dependencies ──────────────────────────────────────────────────────
echo "Installing build tools..."
sudo pacman -Syu --noconfirm
sudo pacman -S --noconfirm \
    base-devel \
    git \
    acme \
    lua \
    lua-posix

# ── WAD file reminder ─────────────────────────────────────────────────────────
echo ""
echo "You need a Doom 1 WAD file.  The shareware version is free:"
echo "  Place doom1.wad in: ~/doom1.wad"
echo "  (or set WAD_PATH in cbm/doom.app.d/main.lua)"
echo ""

# ── build ─────────────────────────────────────────────────────────────────────
echo "Building..."
make
echo ""
echo "Installing..."
make install
echo ""
echo "=== Done! ==="
echo "On the C64/IDUN shell, type:  go doom"
