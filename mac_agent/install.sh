#!/bin/bash
# 把 Mac 监控采集端装成开机自启的 LaunchAgent。
# 装到 ~/Library/Application Support/MacMonitor/ 并自带 venv, 不依赖本工程目录。
set -euo pipefail

LABEL="com.macmonitor.agent"
APP_DIR="$HOME/Library/Application Support/MacMonitor"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
LOG="$HOME/Library/Logs/MacMonitor.log"
SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$SRC_DIR/mac_monitor.py"
# mac_monitor.py 依赖 temps.py, 两个都得装过去
MODULES=(mac_monitor.py temps.py)

echo "==> 检查 python3"
if ! /usr/bin/python3 -c 'import venv' >/dev/null 2>&1; then
  echo "错误: 系统 python3 不可用。" >&2
  echo "全新的 Mac 上 /usr/bin/python3 只是个占位符, 需要先装命令行工具:" >&2
  echo "    xcode-select --install" >&2
  exit 1
fi

echo "==> 停掉可能已在运行的旧实例"
launchctl bootout "gui/$UID/$LABEL" 2>/dev/null || launchctl unload "$PLIST" 2>/dev/null || true

echo "==> 安装到 $APP_DIR"
mkdir -p "$APP_DIR" "$HOME/Library/LaunchAgents" "$(dirname "$LOG")"
for m in "${MODULES[@]}"; do cp "$SRC_DIR/$m" "$APP_DIR/$m"; done

echo "==> 建 venv 并装依赖 (pyserial, psutil)"
if [ ! -x "$APP_DIR/venv/bin/python3" ]; then
  /usr/bin/python3 -m venv "$APP_DIR/venv"
fi
"$APP_DIR/venv/bin/python3" -m pip install --quiet --upgrade pip
"$APP_DIR/venv/bin/python3" -m pip install --quiet pyserial psutil

echo "==> 写 LaunchAgent"
cat > "$PLIST" <<PLIST_EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>$LABEL</string>
    <key>ProgramArguments</key>
    <array>
        <string>$APP_DIR/venv/bin/python3</string>
        <string>$APP_DIR/mac_monitor.py</string>
    </array>
    <key>RunAtLoad</key>
    <true/>
    <key>KeepAlive</key>
    <true/>
    <!-- 不能用 Background: 那会让 macOS 对它做激进的 CPU/IO 降频, 2Hz 的串口推送会被拖卡 -->
    <key>ProcessType</key>
    <string>Standard</string>
    <key>ThrottleInterval</key>
    <integer>5</integer>
    <key>StandardOutPath</key>
    <string>$LOG</string>
    <key>StandardErrorPath</key>
    <string>$LOG</string>
</dict>
</plist>
PLIST_EOF

echo "==> 加载"
# enable 是必要的: 一旦这个 label 曾被 bootout 过, launchd 会把它记进 disabled 列表,
# 之后光 bootstrap 是拉不起来的
launchctl enable "gui/$UID/$LABEL" 2>/dev/null || true
launchctl bootstrap "gui/$UID" "$PLIST" 2>/dev/null || launchctl load -w "$PLIST"

sleep 3
echo
echo "装好了。"
echo "  日志:   tail -f \"$LOG\""
echo "  状态:   launchctl list | grep macmonitor"
echo "  卸载:   $(dirname "$SRC")/uninstall.sh"
echo
echo "--- 当前日志 ---"
tail -n 8 "$LOG" 2>/dev/null || echo "(还没有输出)"
