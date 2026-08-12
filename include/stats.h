// 主机数据的接收/解析/平滑, 两版固件(深色 monitor / 浅色 monitor_light)共用。
// 抽出来是为了保证协议只有一处定义 —— 复制两份迟早会改跑偏。
#pragma once
#include <Arduino.h>

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
