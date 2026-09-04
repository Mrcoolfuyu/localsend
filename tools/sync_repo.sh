#!/bin/bash
# 将工作区源码同步进 WSL 内的 GitHub 仓库工作目录
set -euo pipefail
SRC=/mnt/c/Users/Mrcool-Joan/WorkBuddy/2026-09-03-15-21-30/localsend-qt
REPO=$HOME/localsend-repo

rsync -a --delete \
    --exclude .git \
    --exclude build \
    --exclude "build-*" \
    --exclude "dist-binary*" \
    --exclude "ls-send*.png" \
    --exclude "ls-recv.png" \
    --exclude "gui-v*.png" \
    --exclude "*.tar.gz" \
    --exclude "*.deb" \
    "$SRC/" "$REPO/"

cd "$REPO"
git add -A
if git diff --cached --quiet; then
    echo "NO_CHANGES"
else
    git commit -m "LocalSend Qt/DTK 原生版 v0.3.5：四 BUG 修复 + UOS deb 打包支持 (com.cnraft.localsend)"
    git push --force origin main 2>&1 | tail -2
fi
git log --oneline -2
