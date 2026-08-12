"""Apple Silicon 温度读取 —— 免 sudo。

走 IOKit 的 IOHIDEventSystemClient 私有 API。这是目前唯一不需要 root 的路子:
  - powermetrics 需要 sudo, 而且 M4 上连 smc sampler 都被移除了
  - ioreg 看不到这些传感器(它们是 IOHIDEvent 服务, 不在 IORegistry 的属性里)
LaunchAgent 里不可能有 sudo, 所以只能走这条。
"""

import ctypes
import ctypes.util
from ctypes import byref, c_double, c_int, c_uint32, c_void_p

_KCFSTRING_UTF8 = 0x08000100
_TEMPERATURE_EVENT = 15
_APPLE_VENDOR_USAGE_PAGE = 0xFF00
_TEMPERATURE_SENSOR_USAGE = 5

# 通电的芯片不可能比室温还凉。低于这个值的读数一定是"传感器在列表里但没数据"
# (整簇核心停靠 / 该 die 区域未接入), 不是真实温度, 绝不能进平均。
_MIN_VALID_C = 20.0
# 同一组传感器内, 离最热点超过这么多度的读数按停靠处理丢掉。
_DEAD_GAP_C = 20.0


class _Reader:
    def __init__(self):
        self.ok = False
        try:
            self.cf = ctypes.CDLL(ctypes.util.find_library("CoreFoundation"))
            self.iokit = ctypes.CDLL(
                "/System/Library/Frameworks/IOKit.framework/IOKit")
            self._bind()
            self._connect()
            self.ok = True
        except Exception:
            self.ok = False

    def _bind(self):
        cf, io = self.cf, self.iokit
        cf.CFStringCreateWithCString.restype = c_void_p
        cf.CFStringCreateWithCString.argtypes = [c_void_p, ctypes.c_char_p, c_uint32]
        cf.CFNumberCreate.restype = c_void_p
        cf.CFNumberCreate.argtypes = [c_void_p, c_int, c_void_p]
        cf.CFDictionaryCreateMutable.restype = c_void_p
        cf.CFArrayGetCount.restype = c_int
        cf.CFArrayGetCount.argtypes = [c_void_p]
        cf.CFArrayGetValueAtIndex.restype = c_void_p
        cf.CFArrayGetValueAtIndex.argtypes = [c_void_p, c_int]
        cf.CFStringGetCStringPtr.restype = ctypes.c_char_p
        cf.CFStringGetCStringPtr.argtypes = [c_void_p, c_uint32]
        cf.CFRelease.argtypes = [c_void_p]

        io.IOHIDEventSystemClientCreate.restype = c_void_p
        io.IOHIDEventSystemClientCreate.argtypes = [c_void_p]
        io.IOHIDEventSystemClientSetMatching.argtypes = [c_void_p, c_void_p]
        io.IOHIDEventSystemClientCopyServices.restype = c_void_p
        io.IOHIDEventSystemClientCopyServices.argtypes = [c_void_p]
        io.IOHIDServiceClientCopyProperty.restype = c_void_p
        io.IOHIDServiceClientCopyProperty.argtypes = [c_void_p, c_void_p]
        io.IOHIDServiceClientCopyEvent.restype = c_void_p
        io.IOHIDServiceClientCopyEvent.argtypes = [c_void_p, c_int, c_int, c_int]
        io.IOHIDEventGetFloatValue.restype = c_double
        io.IOHIDEventGetFloatValue.argtypes = [c_void_p, c_int]

    def _cfstr(self, s):
        return self.cf.CFStringCreateWithCString(None, s.encode(), _KCFSTRING_UTF8)

    def _cfnum(self, n):
        v = c_int(n)
        return self.cf.CFNumberCreate(None, 9, byref(v))  # kCFNumberIntType

    def _connect(self):
        cf, io = self.cf, self.iokit
        self.client = io.IOHIDEventSystemClientCreate(None)
        if not self.client:
            raise RuntimeError("IOHIDEventSystemClientCreate failed")
        d = cf.CFDictionaryCreateMutable(None, 0, None, None)
        cf.CFDictionarySetValue(c_void_p(d), c_void_p(self._cfstr("PrimaryUsagePage")),
                                c_void_p(self._cfnum(_APPLE_VENDOR_USAGE_PAGE)))
        cf.CFDictionarySetValue(c_void_p(d), c_void_p(self._cfstr("PrimaryUsage")),
                                c_void_p(self._cfnum(_TEMPERATURE_SENSOR_USAGE)))
        io.IOHIDEventSystemClientSetMatching(c_void_p(self.client), c_void_p(d))
        self._kproduct = self._cfstr("Product")
        # 服务列表只取一次: 传感器集合在运行期不会变, 每次重新枚举很贵
        self.services = []
        svcs = io.IOHIDEventSystemClientCopyServices(c_void_p(self.client))
        n = cf.CFArrayGetCount(c_void_p(svcs)) if svcs else 0
        for i in range(n):
            s = cf.CFArrayGetValueAtIndex(c_void_p(svcs), i)
            p = io.IOHIDServiceClientCopyProperty(c_void_p(s), c_void_p(self._kproduct))
            nm = cf.CFStringGetCStringPtr(c_void_p(p), _KCFSTRING_UTF8) if p else None
            self.services.append((nm.decode() if nm else f"sensor{i}", s))

    def read_all(self):
        """返回 {传感器名: 摄氏度}。"""
        out = {}
        if not self.ok:
            return out
        io, cf = self.iokit, self.cf
        for name, svc in self.services:
            try:
                ev = io.IOHIDServiceClientCopyEvent(c_void_p(svc), _TEMPERATURE_EVENT, 0, 0)
                if not ev:
                    continue
                v = io.IOHIDEventGetFloatValue(c_void_p(ev), _TEMPERATURE_EVENT << 16)
                cf.CFRelease(c_void_p(ev))
                # 上限挡异常值, 下限挡停靠/未接入的传感器 —— 后者会报接近 0 的数,
                # 混进平均值里能把 CPU 温度拉到个位数
                if _MIN_VALID_C < v < 150.0:
                    out[name] = v
            except Exception:
                continue
        return out


_reader = None


def _get_reader():
    global _reader
    if _reader is None:
        _reader = _Reader()
    return _reader


# 各代芯片的 die 传感器分区, 经实测标定。
#
# 标定方法(见 README): 分别把 CPU 和 GPU 各跑满 30s, 记录每个 tdie 传感器的温升,
# 再各自除以本次测试的平均温升做归一化, 消除"整块 die 一起热"的干扰, 然后比较偏向。
# M4 实测结果:
#   tdie1 / 3 / 8   CPU 偏向 1.29 / 1.42 / 1.66
#   tdie2 / 5 / 7   GPU 偏向 1.85 / 1.18 / 1.37
# 注意 M4 的 CPU 和 GPU 在同一块 die 上, 所以两组读数天然接近, 这是物理现实不是 bug。
DIE_PROFILES = {
    "Apple M4": {"cpu": (1, 3, 8), "gpu": (2, 5, 7)},
}


def _agg(vals):
    """一组同域传感器 -> 一个代表温度。

    不能直接求平均。核数多的机型上经常有整簇核心处于停靠状态, 它们的传感器仍然
    列在 IOHID 里, 读数却掉到接近 0 —— 只要混进平均值, 显示出来的 CPU 温度就会变成
    个位数(实测见过 6°C, 而同一时刻 GPU 是 56°C)。
    先按"离本组最热点不超过 _DEAD_GAP_C"筛掉离群的冷读数, 再对剩下的求平均:
    正常情况下同一块 die 上的传感器彼此不会差 20°C, 所以不会误伤真实读数。
    """
    if not vals:
        return 0.0
    hi = max(vals)
    live = [v for v in vals if v >= hi - _DEAD_GAP_C]
    return sum(live) / len(live)


def _chip_brand():
    import subprocess
    try:
        return subprocess.run(["sysctl", "-n", "machdep.cpu.brand_string"],
                              capture_output=True, text=True, timeout=3).stdout.strip()
    except Exception:
        return ""


_profile_cache = None


def cpu_gpu_temp():
    """(CPU 温度, GPU 温度) 摄氏度, 拿不到返回 (0.0, 0.0)。

    传感器命名各代不同:
      M1/M2  -> 'pACC MTR Temp Sensor%d'(性能核) / 'eACC ...'(能效核) / 'GPU MTR Temp Sensor%d'
                有显式标签, 直接按名字分
      M3/M4  -> 统称 'PMU tdie%d', 没有任何 CPU/GPU 标签, 只能靠实测标定分区
    未知芯片则退回: CPU 取所有 die 传感器均值(即"多核平均温度"), GPU 取 die 峰值。

    三条路都用 _agg 而不是裸的 mean: 停靠核心簇的传感器会报接近 0 的假读数,
    直接求平均会把 CPU 温度拉成个位数。
    """
    global _profile_cache
    r = _get_reader()
    t = r.read_all()
    if not t:
        return 0.0, 0.0

    # 1) M1/M2 有显式命名, 最可靠
    cpu = _agg([v for k, v in t.items() if "ACC MTR Temp" in k])
    gpu = _agg([v for k, v in t.items() if k.startswith("GPU")])
    if cpu and gpu:
        return round(cpu, 1), round(gpu, 1)

    die = {k: v for k, v in t.items() if k.startswith("PMU tdie")}
    if not die:
        return 0.0, 0.0

    # 2) 本机芯片有标定过的分区表
    if _profile_cache is None:
        _profile_cache = DIE_PROFILES.get(_chip_brand(), {})
    prof = _profile_cache
    if prof:
        c = [die[f"PMU tdie{i}"] for i in prof["cpu"] if f"PMU tdie{i}" in die]
        g = [die[f"PMU tdie{i}"] for i in prof["gpu"] if f"PMU tdie{i}" in die]
        if c and g:
            return round(_agg(c), 1), round(_agg(g), 1)

    # 3) 未标定的芯片: 平均值当 CPU, 峰值当 GPU —— 不精确但不会误导太多
    return round(_agg(list(die.values())), 1), round(max(die.values()), 1)


if __name__ == "__main__":
    rd = _get_reader()
    print(f"reader ok = {rd.ok}, 传感器数 = {len(rd.services)}")
    all_t = rd.read_all()
    for k in sorted(all_t):
        print(f"  {k:34s} {all_t[k]:6.1f}")
    print()
    print("cpu, gpu =", cpu_gpu_temp())
