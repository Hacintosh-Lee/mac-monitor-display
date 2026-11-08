// Mac 监控屏 —— 菜单栏控制器
//
// 直接调 launchctl, 不依赖 ctl.sh, 所以 .app 拷到哪都能用, 工程目录删了也不影响。
// LSUIElement=true, 不占 Dock 图标。

import Cocoa

let kLabel   = "com.macmonitor.agent"
let kPlist   = NSHomeDirectory() + "/Library/LaunchAgents/\(kLabel).plist"
let kLog     = NSHomeDirectory() + "/Library/Logs/MacMonitor.log"
let kProject = "__PROJECT_DIR__"        // build.sh 在编译时替换

@discardableResult
func sh(_ cmd: String) -> String {
    let p = Process()
    p.executableURL = URL(fileURLWithPath: "/bin/bash")
    p.arguments = ["-lc", cmd]
    let pipe = Pipe()
    p.standardOutput = pipe
    p.standardError = pipe
    do { try p.run() } catch { return "" }
    let d = pipe.fileHandleForReading.readDataToEndOfFile()
    p.waitUntilExit()
    return String(data: d, encoding: .utf8) ?? ""
}

enum State {
    case running        // 服务在跑, 设备也在
    case noDevice       // 服务在跑, 但没插设备
    case stopped        // 服务没跑
}

struct Status {
    var state: State
    var pid: String
    var port: String
    var lastLog: String
}

func readStatus() -> Status {
    let uid = getuid()
    let list = sh("launchctl list | grep '\(kLabel)' || true")
    var pid = ""
    var alive = false
    // launchctl list 的格式: <PID>\t<上次退出码>\t<label>;  没在跑时 PID 是 "-"
    if !list.isEmpty {
        let cols = list.trimmingCharacters(in: .whitespacesAndNewlines)
                       .components(separatedBy: "\t")
        if cols.count >= 1, cols[0] != "-", Int(cols[0]) != nil {
            pid = cols[0]
            alive = true
        }
    }
    _ = uid
    let port = sh("ls /dev/cu.usbmodem* 2>/dev/null | head -n1")
                 .trimmingCharacters(in: .whitespacesAndNewlines)
    let log = sh("tail -n 1 '\(kLog)' 2>/dev/null")
                .trimmingCharacters(in: .whitespacesAndNewlines)

    let st: State = !alive ? .stopped : (port.isEmpty ? .noDevice : .running)
    return Status(state: st, pid: pid, port: port, lastLog: log)
}

func startAgent() {
    let uid = getuid()
    sh("launchctl enable gui/\(uid)/\(kLabel) 2>/dev/null; " +
       "launchctl bootstrap gui/\(uid) '\(kPlist)' 2>/dev/null || " +
       "launchctl load -w '\(kPlist)' 2>/dev/null")
}

func stopAgent() {
    let uid = getuid()
    sh("launchctl bootout gui/\(uid)/\(kLabel) 2>/dev/null || " +
       "launchctl unload '\(kPlist)' 2>/dev/null")
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    var item: NSStatusItem!
    var timer: Timer?
    var busy: String? = nil      // 正在执行的操作, 期间菜单显示进度
    // 时钟模式是"主动停掉采集端"换来的, 所以服务状态同样是 stopped。
    // 用这个标记把它和"异常停止"区分开, 否则菜单上看起来像出故障了。
    var clockMode = false

    func applicationDidFinishLaunching(_ note: Notification) {
        item = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        // 开机自启时顺手把采集端拉起来 —— 这正是加这个程序的主要目的
        if readStatus().state == .stopped { startAgent() }
        refresh()
        timer = Timer.scheduledTimer(withTimeInterval: 3.0, repeats: true) { [weak self] _ in
            self?.refresh()
        }
    }

    func icon(_ s: State) -> NSImage? {
        // 符号名在旧系统上可能不存在, 取不到就回退到文字标题, 不能让图标变空白
        let name: String
        switch s {
        case .running:  name = "display"
        case .noDevice: name = "display.trianglebadge.exclamationmark"
        case .stopped:  name = "display.slash"
        }
        let img = NSImage(systemSymbolName: name, accessibilityDescription: "Mac Monitor")
            ?? NSImage(systemSymbolName: "display", accessibilityDescription: "Mac Monitor")
        img?.isTemplate = true
        return img
    }

    func refresh() {
        let s = readStatus()
        if let b = item.button {
            if let img = icon(s.state) {
                img.size = NSSize(width: 16, height: 16)
                b.image = img
                b.title = ""
            } else {
                b.image = nil
                b.title = s.state == .running ? "▣" : (s.state == .noDevice ? "▲" : "▢")
            }
        }
        item.menu = buildMenu(s)
    }

    func buildMenu(_ s: Status) -> NSMenu {
        let m = NSMenu()

        let head: String
        switch s.state {
        case .running:  head = "● 运行中"
        case .noDevice: head = "▲ 服务在跑，但没检测到屏幕"
        case .stopped:  head = clockMode ? "◷ 时钟模式" : "○ 已停止"
        }
        let h = NSMenuItem(title: head, action: nil, keyEquivalent: "")
        h.isEnabled = false
        m.addItem(h)

        if let b = busy {
            let bi = NSMenuItem(title: "   \(b)…", action: nil, keyEquivalent: "")
            bi.isEnabled = false
            m.addItem(bi)
        }

        let detail = s.state == .stopped
            ? (clockMode ? "   屏幕正显示时钟" : "   未运行")
            : "   PID \(s.pid)   " + (s.port.isEmpty ? "串口: 未连接" : "串口: " + (s.port as NSString).lastPathComponent)
        let d = NSMenuItem(title: detail, action: nil, keyEquivalent: "")
        d.isEnabled = false
        m.addItem(d)

        if !s.lastLog.isEmpty {
            let l = NSMenuItem(title: "   " + String(s.lastLog.prefix(52)), action: nil, keyEquivalent: "")
            l.isEnabled = false
            m.addItem(l)
        }

        m.addItem(.separator())

        if s.state == .stopped {
            m.addItem(mk("启动", #selector(doStart), "s"))
        } else {
            m.addItem(mk("停止", #selector(doStop), "s"))
        }
        m.addItem(mk("重启", #selector(doRestart), "r"))

        m.addItem(.separator())
        m.addItem(mk(clockMode ? "切换回仪表盘" : "切换到时钟", #selector(doClockToggle), "c"))
        m.addItem(mk("查看日志", #selector(doLog), "l"))
        if kProject.hasPrefix("/") {
            m.addItem(mk("打开项目目录", #selector(doProject), "o"))
        }
        m.addItem(.separator())
        m.addItem(mk("退出", #selector(doQuit), "q"))
        return m
    }

    func mk(_ title: String, _ sel: Selector, _ key: String) -> NSMenuItem {
        let i = NSMenuItem(title: title, action: sel, keyEquivalent: key)
        i.target = self
        return i
    }

    // 所有会阻塞的操作都丢到后台, 否则菜单栏会转菊花
    func async(_ label: String, _ work: @escaping () -> Void) {
        busy = label
        refresh()
        DispatchQueue.global().async {
            work()
            DispatchQueue.main.async { self.busy = nil; self.refresh() }
        }
    }

    @objc func doStart()   { clockMode = false; async("启动中") { startAgent(); sleep(3) } }
    @objc func doStop()    { clockMode = false; async("停止中") { stopAgent();  sleep(1) } }
    @objc func doRestart() { clockMode = false; async("重启中") { stopAgent(); sleep(2); startAgent(); sleep(3) } }

    // 在"仪表盘"和"时钟"之间切换。
    // 固件的规则是: 主机断开约 8 秒后自动进时钟, 来数据就立刻切回仪表盘。
    // 所以这里不需要给设备发任何命令, 停/起采集端就够了。
    @objc func doClockToggle() {
        if clockMode {
            clockMode = false
            async("切换回仪表盘") { startAgent(); sleep(3) }
        } else {
            clockMode = true
            async("切换到时钟（约 8 秒）") { stopAgent(); sleep(9) }
        }
    }

    @objc func doLog()     { sh("open -a Console '\(kLog)'") }
    @objc func doProject() { sh("open '\(kProject)'") }
    @objc func doQuit()    { NSApp.terminate(nil) }
}

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.setActivationPolicy(.accessory)
app.run()
