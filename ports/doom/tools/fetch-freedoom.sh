#!/bin/sh
# Regenerates the committed IWAD (wad/freedoom1.stripped.wad.xz) from a fresh
# Freedoom download.  Not needed for a normal build - the compressed IWAD is
# already in the tree.
set -e
cd "$(dirname "$0")/.."
VER=0.13.0
mkdir -p wad
TMP=$(mktemp -d)
curl -fsSL -o "$TMP/fd.zip" "https://github.com/freedoom/freedoom/releases/download/v$VER/freedoom-$VER.zip"
unzip -q -o "$TMP/fd.zip" "freedoom-$VER/freedoom1.wad" "freedoom-$VER/COPYING.txt" -d "$TMP"
python3 tools/wadstrip.py "$TMP/freedoom-$VER/freedoom1.wad" "$TMP/stripped.wad"
xz -9e -c "$TMP/stripped.wad" > wad/freedoom1.stripped.wad.xz
cp "$TMP/freedoom-$VER/COPYING.txt" wad/FREEDOOM-COPYING.txt
rm -rf "$TMP"
echo "ports/doom/wad/freedoom1.stripped.wad.xz regenerated"
