#!/bin/bash
# 采集端启动器。launchd 直接调它, 而不是调 venv 里的 python。
#
# 为什么要这一层: venv 里的 python3 是一个指向宿主解释器的软链。之前它指向
# /Applications/Xcode.app/... —— Xcode 一升级/移动/卸载, 这条链就断了,
# 开机自启会静默失败, 表现成"屏幕一直不亮", 极难联想到是 Python 没了。
#
# 这里每次启动都先验证 venv 能不能 import 依赖, 不行就挑一个可用的解释器
# 自动重建, 然后再交棒。换 Xcode、重装命令行工具、系统升级都能自己扛过去。
set -uo pipefail

APP="$HOME/Library/Application Support/MacMonitor"
VENV="$APP/venv"
LOG="$HOME/Library/Logs/MacMonitor.log"

log() { echo "[$(date '+%m-%d %H:%M:%S')] $*" >> "$LOG"; }

# 优先用不依赖 Xcode 的解释器。/usr/bin/python3 排最后 —— 它只是个转发器,
# 装了 Xcode 时会被解析到 Xcode 里面去, 正是我们要避开的那条路径。
pick_python() {
  for p in /opt/homebrew/bin/python3 \
           /usr/local/bin/python3 \
           /Library/Developer/CommandLineTools/usr/bin/python3 \
           /usr/bin/python3; do
    if [ -x "$p" ] && "$p" -c 'import venv' >/dev/null 2>&1; then
      echo "$p"; return 0
    fi
  done
  return 1
}

venv_ok() {
  [ -x "$VENV/bin/python3" ] && \
    "$VENV/bin/python3" -c 'import serial, psutil' >/dev/null 2>&1
}

if ! venv_ok; then
  log "venv 不可用，正在重建…"
  if ! BASE=$(pick_python); then
    log "找不到可用的 python3，请运行: xcode-select --install"
    exit 1
  fi
  rm -rf "$VENV"
  "$BASE" -m venv "$VENV"                                    >>"$LOG" 2>&1
  "$VENV/bin/python3" -m pip install --quiet --upgrade pip   >>"$LOG" 2>&1
  "$VENV/bin/python3" -m pip install --quiet pyserial psutil >>"$LOG" 2>&1
  if venv_ok; then
    log "venv 重建完成（基于 $BASE）"
  else
    log "venv 重建失败"
    exit 1
  fi
fi

exec "$VENV/bin/python3" "$APP/mac_monitor.py"
