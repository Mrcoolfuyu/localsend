#!/bin/bash
set -e
rm -rf ~/localsend-repo && mkdir -p ~/localsend-repo/docs
SRC=/mnt/c/Users/Mrcool-Joan/WorkBuddy/2026-09-03-15-21-30/localsend-qt
WS=/mnt/c/Users/Mrcool-Joan/WorkBuddy/2026-09-03-15-21-30
cp -r "$SRC/src" "$SRC/resources" "$SRC/tools" ~/localsend-repo/
cp "$SRC/CMakeLists.txt" "$SRC/build.sh" "$SRC/localsend-qt.desktop" "$SRC/README.md" ~/localsend-repo/
cp "$WS/gui-v034-recv.png" ~/localsend-repo/docs/screenshot-receive.png
cp "$WS/gui-v034.png" ~/localsend-repo/docs/screenshot-send.png
cd ~/localsend-repo
git init -b main -q
git config user.name "Mrcoolfuyu"
git config user.email "Mrcoolfuyu@users.noreply.github.com"
echo "--- 校验关键内容 ---"
grep -c "Mr.cool" README.md
grep -l "spinlogo" src/mainwindow.cpp src/mainwindow.h CMakeLists.txt
ls docs/
git add -A
git commit -qm "LocalSend Qt/DTK 原生版 v0.3.4：与官方 LocalSend 协议 100% 兼容的局域网互传客户端"
git log --oneline
echo "--- 文件清单 ---"
git ls-files
