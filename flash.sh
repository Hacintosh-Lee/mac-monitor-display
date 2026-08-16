#!/bin/bash
# 切换/烧录固件。会自动让开串口再把采集端拉回来 —— 忘了这步 esptool 会占不到口。
#   ./flash.sh monitor    深色科技风 竖版 240x320
#   ./flash.sh land       深色科技风 横版 320x240
#   ./flash.sh light      浅色 Apple 风 竖版
#   加 180 后缀即上下颠倒, 屏幕倒装时用 (monitor180 / land180 / light180)
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

ENV="${1:-}"
case "$ENV" in
  light|monitor|land|monitor180|land180|light180) ;;
  *) echo "用法: $0 {monitor|land|light}[180]"
     echo "  monitor = 深色科技风 竖版 240x320"
     echo "  land    = 深色科技风 横版 320x240"
     echo "  light   = 浅色 Apple 风 竖版"
     echo "  加 180 后缀 = 画面上下颠倒, 屏幕倒装时用 (monitor180 / land180 / light180)"
     exit 1 ;;
esac

CTL=./mac_agent/ctl.sh
echo "==> 停采集端, 让出串口"
"$CTL" stop >/dev/null 2>&1
sleep 1

echo "==> 烧录 $ENV"
if ! .venv/bin/pio run -e "$ENV" -t upload 2>&1 | tail -n 3; then
  echo "烧录失败" >&2
fi

echo "==> 拉回采集端"
sleep 1
"$CTL" start >/dev/null 2>&1
sleep 4
"$CTL" status
