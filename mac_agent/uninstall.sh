#!/bin/bash
# 卸载 Mac 监控采集端
set -uo pipefail

LABEL="com.macmonitor.agent"
APP_DIR="$HOME/Library/Application Support/MacMonitor"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"

launchctl bootout "gui/$UID/$LABEL" 2>/dev/null || launchctl unload "$PLIST" 2>/dev/null || true
rm -f "$PLIST"
rm -rf "$APP_DIR"
echo "已卸载。日志 ~/Library/Logs/MacMonitor.log 保留着, 要删自己删。"
