#!/bin/bash
# 在 Xvfb 中启动 localsend-qt 并截图，用于无显示器环境下的 UI 冒烟验证。
# 用法：bash tools/gui_screenshot.sh [输出png路径]
set -u
OUT="${1:-/tmp/localsend-gui.png}"
DISP=:77
BIN=/opt/localsend-qt/localsend-qt

pkill -x Xvfb >/dev/null 2>&1
pkill -x localsend-qt >/dev/null 2>&1
sleep 1

Xvfb "$DISP" -screen 0 1200x820x24 >/tmp/xvfb.log 2>&1 &
XPID=$!
sleep 2

DISPLAY="$DISP" "$BIN" >/tmp/gui-shot.log 2>&1 &
GPID=$!
sleep 8

echo "== 进程状态 =="
if kill -0 "$GPID" 2>/dev/null; then echo "localsend-qt 存活: OK (pid $GPID)"; else echo "localsend-qt 存活: FAIL"; fi

echo "== 截图 =="
OK=0
if command -v import >/dev/null 2>&1; then
  DISPLAY="$DISP" import -window root "$OUT" && OK=1
fi
if [ "$OK" -eq 0 ] && command -v scrot >/dev/null 2>&1; then
  DISPLAY="$DISP" scrot "$OUT" && OK=1
fi
if [ "$OK" -eq 0 ] && command -v xwd >/dev/null 2>&1; then
  DISPLAY="$DISP" xwd -root -silent > /tmp/localsend-gui.xwd && OK=1
  echo "已用 xwd 抓取：/tmp/localsend-gui.xwd（需 convert 转 png）"
fi

if [ "$OK" -eq 1 ] && [ -f "$OUT" ]; then
  echo "截图已保存：$OUT  ($(stat -c%s "$OUT") 字节)"
else
  echo "截图失败（未找到 import/scrot/xwd，或抓取出错）"
fi

echo "== 窗口列表 =="
DISPLAY="$DISP" xdotool search --onlyvisible --name "" 2>/dev/null | head -5
DISPLAY="$DISP" wmctrl -l 2>/dev/null | head -5

kill "$GPID" >/dev/null 2>&1
kill "$XPID" >/dev/null 2>&1
sleep 1
echo DONE
