#!/bin/bash
set -e
WS=/mnt/c/Users/Mrcool-Joan/WorkBuddy/2026-09-03-15-21-30
SRC=$WS/localsend-qt
PKG=~/localsend-qt-v0.3.4-aarch64-UOS
rm -rf "$PKG" && mkdir -p "$PKG"
cp "$SRC/dist-binary-localsend-qt" "$PKG/localsend-qt"
chmod 755 "$PKG/localsend-qt"
cp "$SRC/localsend-qt.desktop" "$PKG/"
cp "$SRC/resources/icons/logo-512.png" "$PKG/"
cp "$SRC/README.md" "$PKG/"
cd ~ && tar czf localsend-qt-v0.3.4-aarch64-UOS.tar.gz "$(basename $PKG)"
ls -la ~/localsend-qt-v0.3.4-aarch64-UOS.tar.gz
tar tzf ~/localsend-qt-v0.3.4-aarch64-UOS.tar.gz
