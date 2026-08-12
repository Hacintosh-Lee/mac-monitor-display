#!/bin/bash
# 采集端控制脚本。把 launchctl 那套语法包起来, 免得手敲出错。
#   ./ctl.sh status              查看状态
#   ./ctl.sh start|stop|restart  启停
#   ./ctl.sh log                 跟踪日志
#   ./ctl.sh label "文字"        临时预览一个机型名(看完自动恢复)
#   ./ctl.sh port                当前串口设备
set -uo pipefail

LABEL="com.macmonitor.agent"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
LOG="$HOME/Library/Logs/MacMonitor.log"

start() { launchctl enable "gui/$UID/$LABEL" 2>/dev/null; launchctl bootstrap "gui/$UID" "$PLIST" 2>/dev/null || launchctl load -w "$PLIST" 2>/dev/null; }
stop()  { launchctl bootout "gui/$UID/$LABEL" 2>/dev/null || launchctl unload "$PLIST" 2>/dev/null; }

find_port() {
  # C3 每次复位都会重新枚举, 路径会变(21201 -> 212401 -> 212101), 所以每次都现找
  ls /dev/cu.usbmodem* 2>/dev/null | head -n1
}

case "${1:-status}" in
  start)   start; sleep 2; exec "$0" status ;;
  stop)    stop;  echo "已停止 (记得 ./ctl.sh start 拉回来)" ;;
  restart) stop; sleep 1; start; sleep 3; exec "$0" status ;;
  log)     tail -f "$LOG" ;;
  port)    find_port || echo "(设备不在)" ;;

  status)
    line=$(launchctl list | grep -i macmonitor)
    if [ -n "$line" ]; then
      echo "服务:   运行中  (PID/退出码/标签: $line)"
    else
      echo "服务:   未运行  -> ./ctl.sh start"
    fi
    p=$(find_port)
    echo "串口:   ${p:-(设备不在, 检查 USB 线是否只供电不传数据)}"
    echo "日志:   $(tail -n 1 "$LOG" 2>/dev/null || echo '(空)')"
    ;;

  label)
    txt="${2:-}"
    [ -z "$txt" ] && { echo "用法: ./ctl.sh label \"MacBook Neo A18 Pro\""; exit 1; }
    p=$(find_port)
    [ -z "$p" ] && { echo "设备不在, 没法预览"; exit 1; }
    # 无论正常结束/报错/Ctrl-C 都要把服务拉回来 —— 之前就是漏了这步把屏幕晾在那
    trap 'echo; echo "恢复服务..."; start; sleep 2; exec "$0" status' EXIT INT TERM
    stop
    sleep 1
    printf '$I,%s\n' "$txt" > "$p"
    echo "已发送标签: $txt"
    echo "屏幕顶栏现在应显示它。按回车恢复正常采集。"
    read -r _
    ;;

  *) echo "用法: $0 {status|start|stop|restart|log|port|label <文字>}"; exit 1 ;;
esac
