#!/bin/bash
# 上传 deb 到 GitHub Release v0.3.5（repo: Mrcoolfuyu/localsend）
set -euo pipefail
DEB="$HOME/com.cnraft.localsend_0.3.5_arm64.deb"
[ -f "$DEB" ] || { echo "未找到 $DEB，先运行 build_deb.sh" >&2; exit 1; }

cd ~/localsend-repo
gh auth setup-git 2>/dev/null || true

NOTES=$(cat <<'EOF'
## LocalSend UOS 原生版 v0.3.5（arm64 / aarch64）

### 新增 / 修复
- 发送列表支持按选中项过滤发送（只发选中的文件）
- 文本消息确认框精简：同意 / 拒绝 / 一律拒绝（一律拒绝后重启前屏蔽该发送方）
- 接收列表正确显示发送方名称；去除冗余的进度/完成条目
- 文本消息弹窗：标题正常、文字可选中复制、新增「复制全部」按钮
- 修复 DTK 标题栏竖线；修复小窗口时发送按钮溢出
- 刷新时主动探测，离线设备立即消失
- 设备指纹持久化：反复启动不再产生多个同名客户端

### 安装（UOS / deepin，arm64）
```bash
sudo apt install ./com.cnraft.localsend_0.3.5_arm64.deb
```
自动安装全部依赖；安装后启动器自动出现 LocalSend 快捷方式，桌面同时生成图标。
（UOS 20 aarch64 实测环境：Qt 5.11.3 + DTK）
EOF
)

if gh release view "v${VERSION}" >/dev/null 2>&1; then
    gh release upload "v${VERSION}" "$DEB" --clobber
else
    gh release create "v${VERSION}" "$DEB" --title "v${VERSION}" --notes "$NOTES"
fi

echo "===== 上传完成 ====="
gh release view "v${VERSION}" --json name,assets --jq '.name, (.assets[] | .name + "  " + .size|tostring)' 2>/dev/null || gh release view "v${VERSION}"
