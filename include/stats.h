// 主机数据的接收/解析/平滑, 两版固件(深色 monitor / 浅色 monitor_light)共用。
// 抽出来是为了保证协议只有一处定义 —— 复制两份迟早会改跑偏。
#pragma once
#include <Arduino.h>
#include "timekeeper.h"

// 能显示的最大核心数。以前是 32, 但 Mac Pro / 大核数机型的逻辑 CPU 会超过这个数,
// 超了之后 ncore 被硬钳到 32, 屏幕上就画出 32 根柱子 —— 核数显示是错的。
// 64 覆盖了目前所有 Mac (Intel Mac Pro 28 核 56 线程是历史最高)。
#define MAXCORE 64

// 温度合理区间。通电的芯片不会低于室温, 也不会超过 125°C;
// 落在区间外的一律当"读不到", 显示端会直接不画, 而不是画一个离谱的数。
static inline float saneTemp(float t) {
  return (t >= 15.0f && t <= 125.0f) ? t : 0.0f;
}

struct Stats {
  float cpu = 0, gpu = 0;
  float memPct = 0, memUsed = 0, memTotal = 0, swapPct = 0;
  float diskPct = 0, diskUsed = 0, diskTotal = 0;
  float rx = 0, tx = 0;
  float load1 = 0;
  long upMin = 0;
  char clock[8] = "--:--";
  char topName[20] = "-";
  int topCpu = 0;
  float cpuTemp = 0, gpuTemp = 0;
  int ncore = 0;
  float core[MAXCORE] = {0};
};

static Stats st;   // 目标值
static Stats an;   // 动画显示值
static bool haveData = false;
static uint32_t lastRx = 0;

// 机型标签由主机的 $I 帧下发。单独放在 Stats 外面: parseLine 每次会整体覆盖 st,
// 放里面会被 $M 冲掉。
static char g_label[32] = "Mac";

#define HIST 58
static uint8_t hCpu[HIST] = {0};
static uint8_t hNet[HIST] = {0};
static int hPos = 0;
static float netPeak = 64.0f;  // KB/s, 自适应量程

static inline float ease(float cur, float target, float k) {
  return cur + (target - cur) * k;
}

// 温度专用: 只在两端都是有效读数时才缓动。读不到(0)时直接归零让显示端跳过,
// 首次拿到读数时直接跳到位, 避免中途画出一串没有物理意义的过渡值。
static inline float easeTemp(float cur, float target) {
  if (target <= 0.0f) return 0.0f;
  if (cur <= 0.0f) return target;
  return ease(cur, target, 0.12f);
}

static void parseLine(char *line) {
  if (strncmp(line, "$I,", 3) == 0) {
    strncpy(g_label, line + 3, sizeof(g_label) - 1);
    g_label[sizeof(g_label) - 1] = 0;
    lastRx = millis();
    return;
  }
  // $T,<unix秒>,<时区偏移秒> —— 完整时间, 供时钟模式在主机断开后继续走
  if (strncmp(line, "$T,", 3) == 0) {
    char *save = nullptr;
    strtok_r(line, ",", &save);
    char *a = strtok_r(nullptr, ",", &save);
    char *b = strtok_r(nullptr, ",", &save);
    if (a && b) tkSetFromHost((time_t)atoll(a), atol(b));
    lastRx = millis();
    return;
  }
  if (strncmp(line, "$M,", 3) != 0) return;

  char *save = nullptr;
  char *tok = strtok_r(line, ",", &save);  // "$M"
  int idx = 0;
  int coreSeen = 0;   // 实际收到几个核心字段, 用来校验整帧完整性
  Stats n;
  while ((tok = strtok_r(nullptr, ",", &save)) != nullptr) {
    switch (idx) {
      case 0:  n.cpu = atof(tok); break;
      case 1:  n.gpu = atof(tok); break;
      case 2:  n.memPct = atof(tok); break;
      case 3:  n.memUsed = atof(tok); break;
      case 4:  n.memTotal = atof(tok); break;
      case 5:  n.swapPct = atof(tok); break;
      case 6:  n.diskPct = atof(tok); break;
      case 7:  n.diskUsed = atof(tok); break;
      case 8:  n.diskTotal = atof(tok); break;
      case 9:  n.rx = atof(tok); break;
      case 10: n.tx = atof(tok); break;
      case 11: n.load1 = atof(tok); break;
      case 12: n.upMin = atol(tok); break;
      case 13: strncpy(n.clock, tok, sizeof(n.clock) - 1); break;
      case 14: strncpy(n.topName, tok, sizeof(n.topName) - 1); break;
      case 15: n.topCpu = atoi(tok); break;
      case 16: n.cpuTemp = saneTemp(atof(tok)); break;
      case 17: n.gpuTemp = saneTemp(atof(tok)); break;
      case 18: n.ncore = atoi(tok); break;
      default: {
        int ci = idx - 19;
        if (ci >= 0) {
          coreSeen++;
          if (ci < MAXCORE) n.core[ci] = atof(tok);
        }
        break;
      }
    }
    idx++;
  }
  if (idx < 19) return;  // 半行/脏数据
  // 主机声明了 ncore 个核心就必然跟着 ncore 个数值。对不上说明这一帧被截断或串了位,
  // 此时 ncore / 温度这些字段拿到的都是别的字段的值 —— 整帧丢掉, 宁可少刷一次。
  if (n.ncore < 1 || n.ncore != coreSeen) return;
  if (n.ncore > MAXCORE) n.ncore = MAXCORE;  // 画得下多少画多少
  st = n;
  haveData = true;
  lastRx = millis();

  hCpu[hPos] = (uint8_t)(st.cpu > 100 ? 100 : st.cpu);
  float tot = st.rx + st.tx;
  if (tot > netPeak) netPeak = tot;
  hNet[hPos] = (uint8_t)(100.0f * tot / netPeak);
  hPos = (hPos + 1) % HIST;
  netPeak = netPeak * 0.995f + 64.0f * 0.005f;  // 慢慢收回量程
  if (netPeak < 64.0f) netPeak = 64.0f;
}

static void pollSerial() {
  // 64 核的 $M 帧约 380 字节, 留足余量; 缓冲区小于整帧会让每一帧都被当成溢出丢弃
  static char buf[768];
  static int len = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (len > 0) {
        buf[len] = 0;
        parseLine(buf);
        len = 0;
      }
    } else if (len < (int)sizeof(buf) - 1) {
      buf[len++] = c;
    } else {
      len = 0;  // 溢出丢弃
    }
  }
}

// 把目标值平滑地推向显示值, 让指针/柱子动起来不跳变
static void animate() {
  const float k = 0.18f;
  an.cpu = ease(an.cpu, st.cpu, k);
  an.gpu = ease(an.gpu, st.gpu, k);
  an.memPct = ease(an.memPct, st.memPct, k);
  an.memUsed = ease(an.memUsed, st.memUsed, k);
  an.memTotal = st.memTotal;
  an.diskPct = ease(an.diskPct, st.diskPct, k);
  an.diskUsed = ease(an.diskUsed, st.diskUsed, k);
  an.diskTotal = st.diskTotal;
  an.rx = ease(an.rx, st.rx, 0.3f);
  an.tx = ease(an.tx, st.tx, 0.3f);
  an.load1 = ease(an.load1, st.load1, k);
  // 温度变化慢, 平滑得更狠一点。但缓动只在"两个都是有效读数"时做:
  // 从 0 缓到 59 的过程中屏幕会依次显示 7°C / 14°C / 22°C..., 那几帧全是假数字。
  an.cpuTemp = easeTemp(an.cpuTemp, st.cpuTemp);
  an.gpuTemp = easeTemp(an.gpuTemp, st.gpuTemp);
  for (int i = 0; i < st.ncore; i++) an.core[i] = ease(an.core[i], st.core[i], 0.25f);
  strncpy(an.clock, st.clock, sizeof(an.clock) - 1);
}

static void fmtRate(float kbs, char *out, size_t n) {
  if (kbs >= 1024.0f) snprintf(out, n, "%.1f MB/s", kbs / 1024.0f);
  else if (kbs >= 10.0f) snprintf(out, n, "%.0f KB/s", kbs);
  else snprintf(out, n, "%.1f KB/s", kbs);
}


// ---------------------------------------------------------------------------
// USB 看门狗 —— 替代"拔插线"
//
// 故障现象: 开机后采集端连上串口, 但一个字节也送不进来, 每 39 秒超时重连一次,
// 循环几百轮都好不了, 唯一有效的恢复手段是拔插 USB 线(= 给芯片断电)。
// 主机侧的"重开串口"不会复位芯片, 所以重连多少次对面都还是那个卡死的外设。
//
// 难点: 设备分不清"USB 卡死"和"用户主动切到时钟模式"。
// Serial 的真假只取决于有没有收到 USB 的 SOF 帧, 也就是"线插在一台通电主机上",
// 跟主机有没有打开串口无关 —— 两种情况下设备看到的都是「插着、没数据」。
//
// 所以不去区分, 而是让重启本身变得无害且有上限:
//   1. 时间存在 RTC 内存里, 重启不丢, 屏幕最多黑一下就回到时钟
//   2. 阈值拉到 10 分钟, 正常待机不会频繁打扰
//   3. 最多连续自救 3 次。真是卡死的话一两次就好了; 如果是用户故意停了采集端,
//      试满 3 次就彻底安静下来当时钟, 不再反复重启
#define WDT_IDLE_MS      600000UL   // 插着主机但多久没数据就自救
#define WDT_MAX_RESTARTS 3

RTC_DATA_ATTR static uint8_t g_wdtCount;   // 连续自救次数, 软重启后保留

static void usbWatchdog() {
  static uint32_t lastActivity = 0;
  static uint32_t seenRx = 0;
  static bool init = false;
  uint32_t now = millis();
  if (!init) { init = true; lastActivity = now; }

  if (lastRx != seenRx) {          // 收到新数据: 清计时和自救计数
    seenRx = lastRx;
    lastActivity = lastRx;
    g_wdtCount = 0;
  }

  // 这里曾经有一道 `if (!Serial) return;` 的闸门, 本意是"没插在主机上就别重启",
  // 结果恰恰在真正需要时把看门狗关掉了:
  // Serial 的真假依赖 SOF 中断, 而 USB-Serial-JTAG 外设一旦底层卡死, SOF 也停了,
  // Serial 变假 -> 每轮都把计时器清零 -> 看门狗永远走不到阈值。
  // 实测卡死时连 esptool 都握不上手(DTR/RTS 复位都送不进芯片), 主机侧毫无办法,
  // 只能靠设备自己重启。所以不再看 Serial, 无条件计时。
  //
  // 误伤待机的代价已经很低: 时间存在 RTC 内存里重启不丢(屏幕只黑一下),
  // 而且最多连续自救 3 次就彻底安静。

  if (now - lastActivity <= WDT_IDLE_MS) return;

  if (g_wdtCount >= WDT_MAX_RESTARTS) {
    lastActivity = now;            // 放弃自救, 安静当时钟
    return;
  }
  g_wdtCount++;
  tkSaveForRestart();              // 把时间带过重启
  // 注意: 这里不要 Serial.printf/flush。主机(采集端)不读串口, 设备的发送缓冲
  // 迟早填满, 届时写入会阻塞 —— 在自救路径上卡住就彻底没救了。
  delay(50);
  ESP.restart();
}
