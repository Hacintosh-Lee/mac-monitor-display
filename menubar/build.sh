#!/bin/bash
# 编译菜单栏控制器并安装成开机自启。
#   ./menubar/build.sh
# 卸载见本文件末尾提示。
set -euo pipefail

SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SRC_DIR/.." && pwd)"

APP_NAME="MacMonitorMenu"
APP_DIR="$HOME/Applications/$APP_NAME.app"
LABEL="com.macmonitor.menubar"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
BUILD="$(mktemp -d)"
trap 'rm -rf "$BUILD"' EXIT

echo "==> 检查 swiftc"
if ! xcrun --find swiftc >/dev/null 2>&1; then
  echo "错误: 找不到 swiftc。需要安装 Xcode 命令行工具:" >&2
  echo "    xcode-select --install" >&2
  exit 1
fi

echo "==> 编译"
# 把项目路径编进去, 这样"打开项目目录"菜单才知道去哪
sed "s|__PROJECT_DIR__|$PROJECT_DIR|" "$SRC_DIR/$APP_NAME.swift" > "$BUILD/$APP_NAME.swift"
xcrun swiftc -O -o "$BUILD/$APP_NAME" "$BUILD/$APP_NAME.swift" \
  -framework Cocoa -target "$(uname -m)-apple-macos13.0"

echo "==> 打包 .app"
# 先停掉可能正在运行的旧版本, 否则覆盖的是正在执行的二进制
launchctl bootout "gui/$UID/$LABEL" 2>/dev/null || true
pkill -f "$APP_NAME" 2>/dev/null || true
sleep 1

rm -rf "$APP_DIR"
mkdir -p "$APP_DIR/Contents/MacOS"
cp "$BUILD/$APP_NAME" "$APP_DIR/Contents/MacOS/$APP_NAME"
cat > "$APP_DIR/Contents/Info.plist" <<PLIST_EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key><string>Mac Monitor</string>
    <key>CFBundleDisplayName</key><string>Mac Monitor</string>
    <key>CFBundleIdentifier</key><string>$LABEL</string>
    <key>CFBundleExecutable</key><string>$APP_NAME</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>1.0</string>
    <key>LSMinimumSystemVersion</key><string>13.0</string>
    <!-- 只在菜单栏出现, 不占 Dock 图标 -->
    <key>LSUIElement</key><true/>
</dict>
</plist>
PLIST_EOF

echo "==> 注册开机自启"
mkdir -p "$HOME/Library/LaunchAgents"
cat > "$PLIST" <<PLIST_EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key><string>$LABEL</string>
    <key>ProgramArguments</key>
    <array><string>$APP_DIR/Contents/MacOS/$APP_NAME</string></array>
    <key>RunAtLoad</key><true/>
    <!-- 不用 KeepAlive: 菜单里点了"退出"就该真的退出, 而不是被立刻拉起来 -->
    <key>KeepAlive</key><false/>
    <key>ProcessType</key><string>Interactive</string>
</dict>
PLIST_EOF
echo "</plist>" >> "$PLIST"

launchctl enable "gui/$UID/$LABEL" 2>/dev/null || true
launchctl bootstrap "gui/$UID" "$PLIST" 2>/dev/null || launchctl load -w "$PLIST"

sleep 2
echo
if pgrep -f "$APP_NAME" >/dev/null; then
  echo "装好了，看一眼菜单栏右上角。"
else
  echo "已安装但进程没起来，手动打开一次: open '$APP_DIR'" >&2
fi
echo
echo "  应用:  $APP_DIR"
echo "  自启:  $PLIST"
echo "  卸载:  launchctl bootout gui/\$UID/$LABEL; rm -rf '$APP_DIR' '$PLIST'"
