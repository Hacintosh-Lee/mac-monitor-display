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
MODULES=(mac_monitor.py temps.py run.sh)

echo "==> 挑选解释器"
# 优先不依赖 Xcode 的。/usr/bin/python3 只是转发器, 装了 Xcode 会被解析进 Xcode.app,
# 那条路径会随 Xcode 升级/卸载而断掉, 所以排最后。
BASE_PY=""
for p in /opt/homebrew/bin/python3 /usr/local/bin/python3 \
         /Library/Developer/CommandLineTools/usr/bin/python3 /usr/bin/python3; do
  if [ -x "$p" ] && "$p" -c 'import venv' >/dev/null 2>&1; then BASE_PY="$p"; break; fi
done
if [ -z "$BASE_PY" ]; then
  echo "错误: 找不到可用的 python3。先装命令行工具:" >&2
  echo "    xcode-select --install" >&2
  exit 1
fi
echo "    使用 $BASE_PY"
case "$BASE_PY" in
  *Xcode.app*) echo "    提示: 这个解释器在 Xcode 里, Xcode 变动时启动器会自动重建 venv" ;;
esac

echo "==> 停掉可能已在运行的旧实例"
launchctl bootout "gui/$UID/$LABEL" 2>/dev/null || launchctl unload "$PLIST" 2>/dev/null || true

echo "==> 安装到 $APP_DIR"
mkdir -p "$APP_DIR" "$HOME/Library/LaunchAgents" "$(dirname "$LOG")"
for m in "${MODULES[@]}"; do cp "$SRC_DIR/$m" "$APP_DIR/$m"; done
chmod +x "$APP_DIR/run.sh"

echo "==> 建 venv 并装依赖 (pyserial, psutil)"
# 旧 venv 可能指向已经不可靠的解释器, 直接重建, 反正很快
rm -rf "$APP_DIR/venv"
"$BASE_PY" -m venv "$APP_DIR/venv"
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
        <string>/bin/bash</string>
        <string>$APP_DIR/run.sh</string>
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
