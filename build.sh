#!/bin/sh
# localsend-qt 在 UOS 20 (aarch64) 上的构建脚本
# 用法： sudo ./build.sh    或  以 mrcool 身份在有 sudo 的环境运行
set -e

echo "==> 安装 Qt5 + DTK 构建依赖（需要网络/管理员权限）"
if command -v sudo >/dev/null 2>&1; then
  sudo apt-get update -qq
  sudo apt-get install -y qt5-default qtbase5-dev qt5-qmake qttools5-dev-tools \
                          libdtkcore-dev libdtkgui-dev libdtkwidget-dev \
                          cmake g++ make
else
  apt-get update -qq
  apt-get install -y qt5-default qtbase5-dev qt5-qmake qttools5-dev-tools \
                      libdtkcore-dev libdtkgui-dev libdtkwidget-dev \
                      cmake g++ make
fi

echo "==> 配置并编译（默认 BUILD_GUI=ON，链接 DTK 原生界面）"
rm -rf build
mkdir -p build
cd build
cmake ..
make -j"$(nproc 2>/dev/null || echo 2)"

echo "==> 完成。二进制位于： $(pwd)/localsend-qt"
echo "    运行： ./build/localsend-qt"
