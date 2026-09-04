#!/bin/bash
# 按 UOS 打包规范组装 deb（可在任意 Linux 主机上交叉组装，不要求 arm64 宿主）
# 用法: build_deb.sh <aarch64二进制路径>
# 产物: ~/com.cnraft.localsend_0.3.5_arm64.deb
set -euo pipefail

APPID=com.cnraft.localsend
VERSION=0.3.5
SRC=/mnt/c/Users/Mrcool-Joan/WorkBuddy/2026-09-03-15-21-30/localsend-qt
TPL="$SRC/tools/deb"
BIN="${1:?用法: build_deb.sh <aarch64二进制路径>}"
STAGE=$(mktemp -d /tmp/debbuild.XXXXXX)

# 校验二进制必须是 aarch64
if ! file "$BIN" | grep -q "ARM aarch64"; then
    echo "错误: $BIN 不是 aarch64 二进制" >&2
    exit 1
fi

# ---- files/ （stage 根即系统根，必须带 /opt/apps 前缀） ----
mkdir -p "$STAGE/opt/apps/$APPID/files/bin"
install -m 755 "$BIN" "$STAGE/opt/apps/$APPID/files/bin/localsend-qt"

# ---- entries/ ----
mkdir -p "$STAGE/opt/apps/$APPID/entries/applications" \
         "$STAGE/opt/apps/$APPID/entries/icons/hicolor/512x512/apps"
install -m 644 "$TPL/com.cnraft.localsend.desktop" "$STAGE/opt/apps/$APPID/entries/applications/$APPID.desktop"
install -m 644 "$SRC/resources/icons/logo-512.png" "$STAGE/opt/apps/$APPID/entries/icons/hicolor/512x512/apps/$APPID.png"

# ---- info ----
install -m 644 "$TPL/info" "$STAGE/opt/apps/$APPID/info"

# ---- DEBIAN/ ----
mkdir -p "$STAGE/DEBIAN"
install -m 644 "$TPL/control" "$STAGE/DEBIAN/control"
install -m 755 "$TPL/postinst" "$STAGE/DEBIAN/postinst"
install -m 755 "$TPL/prerm" "$STAGE/DEBIAN/prerm"

# ---- 校验钩子脚本（规范 6.2 强制检查项） ----
if command -v shellcheck >/dev/null 2>&1; then
    shellcheck "$TPL/postinst" "$TPL/prerm"
fi

# ---- 组装 ----
# -Zxz: UOS 20 的老 dpkg 不支持 zstd(control.tar.zst)，必须用 xz
OUT="$HOME/${APPID}_${VERSION}_arm64.deb"
rm -f "$OUT"
dpkg-deb --root-owner-group -Zxz --build "$STAGE" "$OUT" >/dev/null
rm -rf "$STAGE"

echo "===== 构建完成 ====="
ls -la "$OUT"
dpkg-deb -I "$OUT" | head -15
dpkg-deb -c "$OUT" | sed 's/^/  /'
