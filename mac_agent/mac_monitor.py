#!/usr/bin/env python3
"""
Mac -> ESP32 监控数据推送

通过 USB 串口把 Mac 的实时状态推给 ESP32-C3 上的 ST7789 屏幕。
自动寻找 Espressif USB-Serial-JTAG 端口, 掉线自动重连。

协议 (每行一条, \n 结尾):
  $I,<机型标签>                      设备信息, 连上时发一次, 之后每 10s 补发
  $T,<unix秒>,<本地时区偏移秒>       完整时间, 和 $I 同频补发
  $M,cpu,gpu,memPct,memUsedMB,memTotalMB,swapPct,diskPct,diskUsedGB,
     diskTotalGB,rxKBs,txKBs,load1,uptimeMin,HH:MM,topName,topCpu,
     cpuTempC,gpuTempC,ncore,c0..cN

$I / $T 要周期性补发: ESP32 可能在任何时刻重启, 重启后它只有等到下一条才知道
自己接的是哪台机器、现在几点。

$M 里那个 HH:MM 只够显示时钟, 没有日期也没有星期。时钟模式要显示 "SAT 15" 这类
信息, 所以单独用 $T 下发完整的 unix 时间戳 + 时区偏移, 由固件自己算日期和星期 ——
这样固件断开主机后也能靠本地 RTC 继续推算, 不用等下一帧。
"""

import os
import re
import subprocess
import sys
import time

import psutil
from temps import cpu_gpu_temp
import serial
import serial.tools.list_ports

INTERVAL = 0.5          # 推送间隔(秒)
INFO_EVERY = 10.0       # $I 补发间隔(秒)
BAUD = 115200
WRITE_TIMEOUT = 4.0     # 单次写超时(秒)
MAX_WRITE_TIMEOUTS = 8  # 连续写超时多少次才认定真的挂了

_GPU_RE = re.compile(r'"Device Utilization %"=(\d+)')


def data_volume():
    """APFS 的数据卷。Catalina 之前没有这个卷, 回退到根目录。"""
    return "/System/Volumes/Data" if os.path.isdir("/System/Volumes/Data") else "/"


DATA_VOLUME = data_volume()


def log(msg):
    """带时间戳。没有时间戳的日志在排查"服务什么时候没的"时几乎没用。"""
    print(f"[{time.strftime('%m-%d %H:%M:%S')}] {msg}", flush=True)


def _sh(cmd, timeout=6):
    try:
        return subprocess.run(cmd, capture_output=True, text=True,
                              timeout=timeout).stdout.strip()
    except Exception:
        return ""


def short_chip():
    """'Apple M4' -> 'M4';  'Apple M2 Max' -> 'M2 Max';  Intel 的压成 'Intel i7'。"""
    brand = _sh(["sysctl", "-n", "machdep.cpu.brand_string"])
    if not brand:
        return ""
    if brand.startswith("Apple "):
        return brand[6:]
    m = re.search(r"\b(i[3579])[- ]", brand)
    if m:
        return f"Intel {m.group(1)}"
    if "Intel" in brand:
        return "Intel"
    return brand[:12]


def machine_label():
    """拼出 'Mac mini M4' 这样的标签, 换任何一台 Mac 都自动变。"""
    model = ""
    # system_profiler 慢(约 1s), 但只在启动时跑一次
    out = _sh(["system_profiler", "SPHardwareDataType"], timeout=15)
    m = re.search(r"Model Name:\s*(.+)", out)
    if m:
        model = m.group(1).strip()
    if not model:
        # 回退: hw.model 给的是 'Mac16,10' 这种代号, 总比没有强
        model = _sh(["sysctl", "-n", "hw.model"]) or "Mac"

    chip = short_chip()
    label = f"{model} {chip}".strip() if chip else model
    # 放宽长度上限, 显示端会按字体阶梯自适应, 不需要在这里粗暴截断
    return label[:28].replace(",", " ")


def find_port():
    """找 Espressif USB-Serial-JTAG (VID 0x303A)。"""
    if len(sys.argv) > 1:
        return sys.argv[1]
    for p in serial.tools.list_ports.comports():
        if p.vid == 0x303A:
            return p.device
    for p in serial.tools.list_ports.comports():
        if "usbmodem" in p.device or "usbserial" in p.device:
            return p.device
    return None


def gpu_percent() -> float:
    """Apple Silicon GPU 占用率, 无需 sudo。"""
    try:
        out = subprocess.run(
            ["ioreg", "-r", "-d", "1", "-c", "IOAccelerator"],
            capture_output=True, text=True, timeout=2,
        ).stdout
        vals = [int(m) for m in _GPU_RE.findall(out)]
        return float(max(vals)) if vals else 0.0
    except Exception:
        return 0.0


def top_process():
    """占 CPU 最高的进程 (名字, %)。用 ps 比 psutil 轮询便宜得多。"""
    try:
        out = subprocess.run(
            ["ps", "-Aceo", "pcpu,comm", "-r"],
            capture_output=True, text=True, timeout=2,
        ).stdout.splitlines()
        if len(out) >= 2:
            pcpu, _, comm = out[1].strip().partition(" ")
            name = os.path.basename(comm.strip())[:16]
            return name or "-", float(pcpu)
    except Exception:
        pass
    return "-", 0.0


class Sampler:
    def __init__(self):
        self.ncore = psutil.cpu_count(logical=True) or 1
        psutil.cpu_percent(percpu=True)  # 先打底, 第一次调用总是 0
        n = psutil.net_io_counters()
        self._last_net = (n.bytes_recv, n.bytes_sent, time.time())
        # 时间戳必须初始化成"很久以前"。time.monotonic() 在 macOS 上是从进程启动计时的,
        # 初始化成 0 的话头几秒 now-ts < period, 这几个值会一直返回默认值(0 / "-")。
        NEVER = -1e9
        self._gpu_cache = (0.0, NEVER)
        self._top_cache = (("-", 0.0), NEVER)
        self._temp_cache = ((0.0, 0.0), NEVER)

    def _throttled(self, cache_attr, fn, period):
        val, ts = getattr(self, cache_attr)
        now = time.monotonic()
        if now - ts >= period:
            val = fn()
            setattr(self, cache_attr, (val, now))
        return val

    def sample(self) -> str:
        cores = psutil.cpu_percent(percpu=True)
        cpu = sum(cores) / len(cores) if cores else 0.0

        # ioreg / ps 是子进程调用, 限流到 1.5s 一次, 免得自己变成负载
        gpu = self._throttled("_gpu_cache", gpu_percent, 1.5)
        top_name, top_cpu = self._throttled("_top_cache", top_process, 1.5)
        # 温度是 40 个传感器逐个取值, 比较贵; 而且温度本来就变化慢
        cpu_t, gpu_t = self._throttled("_temp_cache", cpu_gpu_temp, 2.0)

        vm = psutil.virtual_memory()
        sw = psutil.swap_memory()
        du = psutil.disk_usage(DATA_VOLUME)

        # psutil 的 used 和 percent 分母不一样, 一起显示会自相矛盾 —— 屏幕上并排放着
        # "168 / 228 GB" 和 "93%", 读的人一算就是 74%, 两个数怎么都对不上:
        #   磁盘: du.used 只算本卷自己的数据(168G), 而 du.total 是整个 APFS 容器(228G),
        #         中间 48G 是系统卷/快照占的; du.percent 又是 used/(used+free)=93%
        #   内存: vm.used 只算 active+wired(3.1G), 而 vm.percent 是 (total-available)/total=85%
        # 统一按 "total - 可用" 算已用量, 百分比就是它除以 total, 三个数自洽;
        # 口径也正好和「关于本机 > 储存空间」「活动监视器 > 内存」一致。
        mem_used = vm.total - vm.available
        mem_pct = 100.0 * mem_used / vm.total if vm.total else 0.0
        disk_used = du.total - du.free
        disk_pct = 100.0 * disk_used / du.total if du.total else 0.0

        n = psutil.net_io_counters()
        now = time.time()
        prx, ptx, pt = self._last_net
        dt = max(now - pt, 1e-3)
        rx = max(n.bytes_recv - prx, 0) / dt / 1024.0
        tx = max(n.bytes_sent - ptx, 0) / dt / 1024.0
        self._last_net = (n.bytes_recv, n.bytes_sent, now)

        load1 = os.getloadavg()[0]
        uptime_min = (time.time() - psutil.boot_time()) / 60.0

        fields = [
            "$M",
            f"{cpu:.1f}", f"{gpu:.1f}",
            f"{mem_pct:.1f}", f"{mem_used / 1048576:.0f}", f"{vm.total / 1048576:.0f}",
            f"{sw.percent:.1f}",
            f"{disk_pct:.1f}", f"{disk_used / 1073741824:.1f}", f"{du.total / 1073741824:.1f}",
            f"{rx:.1f}", f"{tx:.1f}",
            f"{load1:.2f}", f"{uptime_min:.0f}",
            time.strftime("%H:%M"),
            top_name.replace(",", "_"), f"{top_cpu:.0f}",
            f"{cpu_t:.1f}", f"{gpu_t:.1f}",
            str(len(cores)),
        ] + [f"{c:.0f}" for c in cores]
        return ",".join(fields) + "\n"


def main():
    sampler = Sampler()
    label = machine_label()
    log(f"本机: {label}")
    info_line = f"$I,{label}\n".encode("ascii", "replace")

    def time_line():
        now = time.time()
        # 用当前时刻的实际偏移, 自动跟随夏令时
        off = -(time.altzone if time.daylight and time.localtime().tm_isdst else time.timezone)
        return f"$T,{int(now)},{off}\n".encode("ascii", "replace")

    ser = None
    next_info = 0.0
    waiting_logged = False
    wtimeouts = 0

    while True:
        try:
            if ser is None:
                port = find_port()
                if not port:
                    # 只在状态变化时打一次, 否则设备长期不在时日志会无限膨胀
                    if not waiting_logged:
                        log("等待 ESP32 接入...")
                        waiting_logged = True
                    time.sleep(2)
                    continue
                waiting_logged = False
                ser = serial.Serial(port, BAUD, timeout=1, write_timeout=WRITE_TIMEOUT)
                time.sleep(0.3)
                next_info = 0.0        # 新连接立刻补 $I / $T
                wtimeouts = 0
                log(f"已连接 {port}")

            # ESP32 随时可能重启, 定期补发设备信息, 否则它重启后就不知道机型了
            if time.monotonic() >= next_info:
                ser.write(info_line)
                ser.write(time_line())
                next_info = time.monotonic() + INFO_EVERY

            # 把设备发来的东西读掉。只写不读的话, 设备侧的发送缓冲会一直涨,
            # 填满之后它那边的 Serial.print 就会阻塞, 连累主循环不再读串口。
            if ser.in_waiting:
                ser.read(ser.in_waiting)

            ser.write(sampler.sample().encode("ascii", "replace"))
            wtimeouts = 0
            time.sleep(INTERVAL)

        except serial.SerialTimeoutException:
            # 写超时 != 掉线。它只说明 ESP32 一时没把 CDC 缓冲读走 —— 典型场景是它刚
            # 被复位, 正在跑 lcd.init() 和分配精灵, 这一两秒里没人读串口。
            #
            # 这里绝对不能重连: 重开串口会再复位一次 C3, 复位又让它更来不及读, 于是
            # "连上->3秒超时->重连->复位" 无限循环, 永远收敛不了(日志里那 6 轮就是)。
            # 正确做法是丢掉这一帧继续等, 只有连续超时很久才认定真的挂了。
            wtimeouts += 1
            if wtimeouts >= MAX_WRITE_TIMEOUTS:
                log(f"连续 {wtimeouts} 次写超时, 设备可能挂死, 重连")
                try:
                    if ser:
                        ser.close()
                except Exception:
                    pass
                ser = None
                wtimeouts = 0
                time.sleep(2)
            else:
                time.sleep(INTERVAL)

        except (serial.SerialException, OSError) as e:
            log(f"串口断开 ({e}), 2 秒后重连...")
            try:
                if ser:
                    ser.close()
            except Exception:
                pass
            ser = None
            time.sleep(2)
        except KeyboardInterrupt:
            log("收到中断, 退出")
            if ser:
                ser.close()
            return


if __name__ == "__main__":
    main()
