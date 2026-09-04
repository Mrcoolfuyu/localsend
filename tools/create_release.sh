#!/bin/bash
set -e
cd ~
gh release create v0.3.4 \
  --repo Mrcoolfuyu/localsend \
  --title "LocalSend Qt/DTK 原生版 v0.3.4" \
  --notes "基于 LocalSend 协议 v2.2，用 C++ / Qt5 Widgets / 统信 DTK 全新实现的局域网互传客户端，与官方 LocalSend 客户端协议 100% 兼容。

## 下载
- **localsend-qt-v0.3.4-aarch64-UOS.tar.gz**：aarch64（ARM64）预编译二进制，适用于统信 UOS 20 / 华为 L420x / 麒麟 9000C 等环境，无需 GPU/OpenGL

## 安装
\`\`\`sh
tar xzf localsend-qt-v0.3.4-aarch64-UOS.tar.gz
cd localsend-qt-v0.3.4-aarch64-UOS
sudo install -Dm755 localsend-qt /opt/localsend-qt/localsend-qt
sudo install -Dm644 localsend-qt.desktop /usr/share/applications/localsend-qt.desktop
sudo install -Dm644 logo-512.png /usr/share/pixmaps/localsend-qt.png
sudo update-desktop-database /usr/share/applications
\`\`\`

## 主要功能
- 局域网设备自动发现（UDP 组播 224.0.0.167:53317，多网卡自动适配）
- 文件 / 文件夹互传，支持多选设备一次发送、逐文件进度
- 文本消息（与官方 preview 协议一致，对端显示为消息而非文件）
- 接收确认对话框（发送方指纹 / 文件列表 / 消息摘要），防陌生人随意投递
- 完整 HTTPS 支持（自签名证书 + TLS 客户端证书），失败自动回退 http
- 接收页旋转动态 Logo + 设备名展示；接收文件可直接打开 / 打开目录
- 全量运行日志落盘 ~/.local/share/localsend-qt/localsend.log

## 主要变更（v0.3.4）
- 对齐官方消息协议：接收方对纯消息传输回 HTTP 204（不建会话不落盘），发送端正确识别 204 为「已读取并关闭」
- prepare-upload 等待接收方决策超时延长至 30s，403/429 给出明确提示
- 接收页新增旋转动态 Logo 与附近设备名大字展示
- 若干稳定性修复（日志去重、TLS 垃圾字节快速拒绝等）

完整说明见仓库 README。" \
  localsend-qt-v0.3.4-aarch64-UOS.tar.gz
echo "RELEASE_DONE"
gh release view v0.3.4 --repo Mrcoolfuyu/localsend --json name,url,assets --jq '{name, url, assets: [.assets[].name]}'
