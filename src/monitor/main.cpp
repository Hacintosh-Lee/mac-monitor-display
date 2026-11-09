// ============================================================
//  Mac 状态监控屏  ESP32-C3 + ST7789 240x320
//  数据由 mac_agent/mac_monitor.py 通过 USB 串口推送
// ============================================================
#include <Arduino.h>
#include "display.h"
#include "stats.h"
#include "clockface.h"

// 屏幕装反了就把这个覆盖掉, 由 platformio.ini 的 -DSCREEN_ROTATION 传入,
// 不用为了换个方向复制一份源码。
#ifndef SCREEN_ROTATION
#define SCREEN_ROTATION 0  // 0=竖版正向, 2=上下颠倒(屏幕倒装时用)
#endif

// ---------- 版式 (纵向排布, 互不重叠) ----------
#define Y_GAUGE 32
#define H_GAUGE 116
#define Y_CORES 151
#define H_CORES 16
#define Y_USE   170
#define H_USE   70
#define Y_NET   243
#define H_NET   46
#define Y_FOOT  291
#define H_FOOT  29

static LGFX lcd;
// 每块区域一个精灵, 尺寸严格对应 —— LovyanGFX 的 pushSprite 没有局部区域重载,
// 用一个大精灵推局部会把下面的内容一起盖掉。
static LGFX_Sprite sHead(&lcd);    // 顶栏(机型名/时钟)
static LGFX_Sprite sGauge(&lcd);   // 表盘
static LGFX_Sprite sCores(&lcd);   // 核心条
static LGFX_Sprite sUsage(&lcd);   // 内存/磁盘
static LGFX_Sprite sNet(&lcd);     // 网络
static LGFX_Sprite sFoot(&lcd);    // 底栏

// ---------- 配色 ----------
static constexpr uint32_t C_BG     = 0x0A0D14;
static constexpr uint32_t C_CARD   = 0x121A26;
static constexpr uint32_t C_TRACK  = 0x1E2836;
static constexpr uint32_t C_TEXT   = 0xE6EDF3;
static constexpr uint32_t C_DIM    = 0x6B7A8F;
static constexpr uint32_t C_CPU    = 0x00E5FF;
static constexpr uint32_t C_GPU    = 0xB388FF;
static constexpr uint32_t C_MEM    = 0xFFB74D;
static constexpr uint32_t C_DISK   = 0x4DD0A1;
static constexpr uint32_t C_RX     = 0x4FC3F7;
static constexpr uint32_t C_TX     = 0xFF8A80;
static constexpr uint32_t C_WARN   = 0xFFD54F;
static constexpr uint32_t C_HOT    = 0xFF5370;
static constexpr uint32_t C_TEMP   = 0xB8C4D4;  // 温度用色, 比 C_DIM 亮一档才看得清

static inline uint16_t C(uint32_t rgb) {
  return lcd.color565((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}
static uint16_t lerpC(uint32_t a, uint32_t b, float t) {
  if (t < 0) t = 0;
  if (t > 1) t = 1;
  int r = ((a >> 16) & 0xFF) + (((int)((b >> 16) & 0xFF) - (int)((a >> 16) & 0xFF)) * t);
  int g = ((a >> 8) & 0xFF) + (((int)((b >> 8) & 0xFF) - (int)((a >> 8) & 0xFF)) * t);
  int bl = (a & 0xFF) + (((int)(b & 0xFF) - (int)(a & 0xFF)) * t);
  return lcd.color565(r, g, bl);
}
// 负载越高越红
static uint16_t heat(uint32_t base, float pct) {
  if (pct < 60) return C(base);
  if (pct < 85) return lerpC(base, C_WARN, (pct - 60) / 25.0f);
  return lerpC(C_WARN, C_HOT, (pct - 85) / 15.0f);
}

// ---------- 绘制 ----------
static void drawGaugeInto(LGFX_Sprite &s, int cx, int cy, int r, float pct,
                          uint32_t base, const char *label) {
  const int th = 12;
  s.fillArc(cx, cy, r - th, r, 135, 405, C(C_TRACK));
  float sweep = 270.0f * (pct / 100.0f);
  if (sweep > 1.0f) {
    // 沿弧线画出深->亮的渐变, 比单色好看很多
    const int SEG = 24;
    uint16_t tip = heat(base, pct);
    for (int i = 0; i < SEG; i++) {
      float a0 = 135.0f + sweep * i / SEG;
      float a1 = 135.0f + sweep * (i + 1) / SEG + 0.8f;
      if (a1 > 135.0f + sweep) a1 = 135.0f + sweep;
      if (a1 <= a0) continue;
      uint16_t col = (pct < 60)
                         ? lerpC(base & 0x3F3F3F, base, (float)i / SEG)
                         : lerpC(base, (uint32_t)0xFFFFFF, 0.0f);
      s.fillArc(cx, cy, r - th, r, a0, a1, (pct < 60) ? col : tip);
    }
    if (pct >= 60) s.fillArc(cx, cy, r - th, r, 135, 135 + sweep, tip);
    // 内缘高光
    s.fillArc(cx, cy, r - th, r - th + 2, 135, 135 + sweep,
              lerpC(0xFFFFFF, base, 0.5f));
  }
  char num[8];
  snprintf(num, sizeof(num), "%d", (int)(pct + 0.5f));
  s.setTextDatum(middle_center);
  s.setFont(&fonts::FreeSansBold18pt7b);
  s.setTextColor(C(C_TEXT));
  s.drawString(num, cx, cy - 8);
  s.setFont(&fonts::Font2);
  s.setTextColor(C(C_DIM));
  s.drawString("%", cx + (strlen(num) * 11) + 4, cy - 1);

  s.setFont(&fonts::FreeSansBold9pt7b);
  s.setTextColor(heat(base, pct));
  s.drawString(label, cx, cy + r + 12);
}

// 温度显示。内置字体是纯 ASCII, 没有 ° 字形, 度数符号用一个小圆圈自己画。
static void drawTemp(LGFX_Sprite &s, int cx, int cy, float t, uint32_t base) {
  if (t <= 0.0f) return;
  char n[8];
  snprintf(n, sizeof(n), "%d", (int)(t + 0.5f));
  // 用矢量粗体而不是 Font2 点阵: 环内空间小, 点阵在这个尺寸下发虚
  s.setFont(&fonts::FreeSansBold9pt7b);
  int wn = s.textWidth(n), wc = s.textWidth("C");
  int total = wn + 6 + wc + 4;
  int x = cx - total / 2;
  uint16_t col = (t < 70) ? C(C_TEMP)
                          : ((t < 90) ? lerpC(C_TEMP, C_WARN, (t - 70) / 20.0f)
                                      : C(C_HOT));
  s.setTextColor(col);
  s.setTextDatum(middle_left);
  s.drawString(n, x, cy);
  // 度数符号: 实心小环, 比空心圆在小尺寸下清楚
  int dx = x + wn + 5, dy = cy - 5;
  s.fillCircle(dx, dy, 3, col);
  s.fillCircle(dx, dy, 1, C(C_BG));
  s.drawString("C", x + wn + 10, cy);
  s.setTextDatum(middle_center);
}

// 机型名长度差异很大("Mac mini M4" 11 字符 vs "MacBook Neo A18 Pro" 19 字符),
// 按字体阶梯逐级降级去适配剩余宽度; 短名字保持大字号不受影响。
// 实在放不下才截断 —— 内置字体是纯 ASCII, 没有 … 字形, 用三个点代替。
static void drawFittedLabel(LGFX_Sprite &s, const char *txt, int x, int y, int maxW) {
  static const lgfx::IFont *const LADDER[] = {
      &fonts::FreeSansBold9pt7b, &fonts::FreeSans9pt7b, &fonts::Font2};
  for (auto f : LADDER) {
    s.setFont(f);
    if (s.textWidth(txt) <= maxW) {
      s.drawString(txt, x, y);
      return;
    }
  }
  s.setFont(&fonts::Font2);
  char buf[40];
  size_t n = strlen(txt);
  if (n > sizeof(buf) - 4) n = sizeof(buf) - 4;
  while (n > 1) {
    memcpy(buf, txt, n);
    strcpy(buf + n, "...");
    if (s.textWidth(buf) <= maxW) break;
    n--;
  }
  s.drawString(buf, x, y);
}

// 顶栏以前是唯一一块直接往屏上画的区域: 先 fillRect 擦掉整条, 再往上写字。
// 擦和写之间有一段肉眼可见的空窗, 而那条渐变细线又是 240 次 drawPixel ——
// 每次都是一笔独立的 SPI 事务, 把空窗又拉长了一截, 于是每重绘一次就闪一下。
// 改成和其它区域一样: 先画进精灵, 再整块推上屏, 一次成像, 不存在中间态。
static void drawHeader() {
  sHead.fillSprite(C(C_BG));

  // 时钟是顶栏的视觉锚点, 位置和字号固定; 先量出它占多宽
  sHead.setFont(&fonts::FreeSansBold12pt7b);
  int clockW = sHead.textWidth(an.clock);
  sHead.setTextDatum(middle_right);
  sHead.setTextColor(haveData ? C(C_CPU) : C(C_DIM));
  sHead.drawString(an.clock, 232, 15);

  // 剩下的宽度全部让给机型名, 中间留 12px 呼吸位
  int maxW = 232 - clockW - 12 - 8;
  sHead.setTextDatum(middle_left);
  sHead.setTextColor(C(C_TEXT));
  sHead.setClipRect(0, 0, 8 + maxW + 1, 29);  // 兜底: 再长也越不过时钟
  drawFittedLabel(sHead, g_label, 8, 15, maxW);
  sHead.clearClipRect();

  // 顶部渐变细线。画进精灵只是写内存, 不再有 240 次 SPI 往返
  for (int x = 0; x < 240; x++) {
    sHead.drawPixel(x, 29, lerpC(C_CPU, C_GPU, x / 239.0f));
  }

  // 掉线提示也并进这块精灵。以前它在 loop 里每 25ms 往屏上写一次, 又被 400ms 一次的
  // 顶栏重绘擦掉, 两边互相打架, 本身就是一个闪烁源。
  if (millis() - lastRx > 5000) {
    sHead.setTextDatum(middle_center);
    sHead.setFont(&fonts::Font2);
    sHead.setTextColor(C(C_HOT), C(C_BG));
    sHead.drawString(" host offline ", 120, 15);
  }

  sHead.pushSprite(0, 0);
}

static void drawGauges() {
  sGauge.fillSprite(C(C_BG));
  const int cy = 50, r = 42;
  drawGaugeInto(sGauge, 61, cy, r, an.cpu, C_CPU, "CPU");
  drawGaugeInto(sGauge, 179, cy, r, an.gpu, C_GPU, "GPU");
  // 温度放在环内百分比下方, 正好落在内圈空白里
  drawTemp(sGauge, 61, cy + 16, an.cpuTemp, C_CPU);
  drawTemp(sGauge, 179, cy + 16, an.gpuTemp, C_GPU);
  sGauge.pushSprite(0, Y_GAUGE);
}

static void drawCores() {
  sCores.fillSprite(C(C_BG));
  int n = st.ncore > 0 ? st.ncore : 10;
  if (n > MAXCORE) n = MAXCORE;
  // 核数越多间距越窄, 否则 40 核以上会被挤到柱子宽度不足 2px
  int gap = n > 32 ? 1 : (n > 16 ? 2 : 3);
  int w = (236 - gap * (n - 1)) / n;
  if (w < 2) w = 2;
  int x = (240 - (w * n + gap * (n - 1))) / 2;
  for (int i = 0; i < n; i++) {
    float v = an.core[i];
    int h = (int)(15.0f * v / 100.0f + 0.5f);
    int r = w > 4 ? 2 : 1;   // 柱子窄到 2px 时圆角比柱子还宽, 会画不出形状
    sCores.fillRoundRect(x, 0, w, 15, r, C(C_TRACK));
    if (h > 0) sCores.fillRoundRect(x, 15 - h, w, h, r, heat(C_CPU, v));
    x += w + gap;
  }
  sCores.pushSprite(0, Y_CORES);
}

static void drawBar(LGFX_Sprite &s, int y, const char *label, float pct,
                    uint32_t base, const char *sub) {
  // 标签用等宽字体。比例字体下 M 是最宽的字形, "MEM" 有两个 M, 宽度和墨量都比
  // "SSD" 大, 并排看就像字号不同 —— 换成等宽后 3 个字符必然占同样宽度, 从根上一致。
  s.setTextDatum(middle_center);
  s.setFont(&fonts::FreeMonoBold9pt7b);
  s.setTextColor(C(C_DIM));
  s.drawString(label, 28, y + 6);

  int bx = 52, bw = 140;
  s.fillRoundRect(bx, y, bw, 12, 6, C(C_TRACK));
  int fw = (int)(bw * pct / 100.0f + 0.5f);
  if (fw > 4) {
    // 这里以前先用 drawFastVLine 逐列铺一层渐变再盖圆角矩形。两个问题:
    // 一是竖线是方角的, 会盖掉轨道的圆头, 在填充条两端各留下 2 个方角(共 4 个);
    // 二是那层渐变紧接着就被实心圆角矩形整个盖住, 根本没显示过 —— 纯粹是白画 140 次。
    // 直接画一个和轨道同半径的圆角矩形, 两端的圆头就能和轨道严丝合缝。
    if (fw < 12) fw = 12;   // 短于一个圆头的宽度就画不出圆角
    if (fw > bw) fw = bw;
    s.fillRoundRect(bx, y, fw, 12, 6, heat(base, pct));
  }
  char p[8];
  snprintf(p, sizeof(p), "%d%%", (int)(pct + 0.5f));
  s.setTextDatum(middle_right);
  s.setTextColor(C(C_TEXT));
  s.drawString(p, 232, y + 6);

  s.setTextDatum(middle_left);
  s.setFont(&fonts::Font2);
  s.setTextColor(C(C_DIM));
  s.drawString(sub, 52, y + 22);
}

static void drawUsage() {
  sUsage.fillSprite(C(C_BG));
  char sub[32];
  snprintf(sub, sizeof(sub), "%.1f / %.1f GB", an.memUsed / 1024.0f,
           an.memTotal / 1024.0f);
  drawBar(sUsage, 2, "MEM", an.memPct, C_MEM, sub);

  if (an.diskTotal >= 1024.0f)
    snprintf(sub, sizeof(sub), "%.2f / %.2f TB", an.diskUsed / 1024.0f,
             an.diskTotal / 1024.0f);
  else
    snprintf(sub, sizeof(sub), "%.0f / %.0f GB", an.diskUsed, an.diskTotal);
  drawBar(sUsage, 36, "SSD", an.diskPct, C_DISK, sub);

  sUsage.pushSprite(0, Y_USE);
}

static void arrowDown(LGFX_Sprite &s, int x, int y, uint16_t col) {
  s.fillTriangle(x, y + 5, x - 4, y - 2, x + 4, y - 2, col);
  s.fillRect(x - 1, y - 6, 3, 5, col);
}
static void arrowUp(LGFX_Sprite &s, int x, int y, uint16_t col) {
  s.fillTriangle(x, y - 6, x - 4, y + 1, x + 4, y + 1, col);
  s.fillRect(x - 1, y + 1, 3, 5, col);
}

static void drawNet() {
  sNet.fillSprite(C(C_BG));
  char r[16], t[16];
  fmtRate(an.rx, r, sizeof(r));
  fmtRate(an.tx, t, sizeof(t));

  sNet.setFont(&fonts::FreeSansBold9pt7b);
  sNet.setTextDatum(middle_left);
  arrowDown(sNet, 12, 8, C(C_RX));
  sNet.setTextColor(C(C_TEXT));
  sNet.drawString(r, 22, 8);

  sNet.setTextDatum(middle_right);
  sNet.setTextColor(C(C_TEXT));
  sNet.drawString(t, 232, 8);
  arrowUp(sNet, 232 - sNet.textWidth(t) - 10, 8, C(C_TX));

  // 网络 sparkline
  int y0 = 20, h = 26;
  sNet.fillRoundRect(4, y0, 232, h, 3, C(C_CARD));
  for (int i = 0; i < HIST; i++) {
    int idx = (hPos + i) % HIST;
    int v = hNet[idx];
    int bh = v * (h - 4) / 100;
    int x = 6 + i * 4;
    if (bh > 0)
      sNet.fillRect(x, y0 + h - 2 - bh, 3, bh, lerpC(C_RX, C_TX, v / 100.0f));
  }
  sNet.pushSprite(0, Y_NET);
}

static void drawFooter() {
  sFoot.fillSprite(C(C_BG));
  char s[40];
  sFoot.setFont(&fonts::Font2);
  sFoot.setTextDatum(middle_left);

  sFoot.setTextColor(C(C_DIM));
  sFoot.drawString("LOAD", 8, 8);
  sFoot.setTextColor(heat(C_CPU, an.load1 * 10.0f));
  snprintf(s, sizeof(s), "%.2f", an.load1);
  sFoot.drawString(s, 44, 8);

  sFoot.setTextColor(C(C_DIM));
  sFoot.drawString("UP", 94, 8);
  long h = st.upMin / 60, m = st.upMin % 60;
  if (h >= 24) snprintf(s, sizeof(s), "%ldd %ldh", h / 24, h % 24);
  else snprintf(s, sizeof(s), "%ldh %ldm", h, m);
  sFoot.setTextColor(C(C_TEXT));
  sFoot.drawString(s, 116, 8);

  // 最占 CPU 的进程
  sFoot.setTextColor(C(C_GPU));
  sFoot.drawString(st.topName, 8, 24);
  snprintf(s, sizeof(s), "%d%%", st.topCpu);
  sFoot.setTextDatum(middle_right);
  sFoot.setTextColor(C(C_DIM));
  sFoot.drawString(s, 232, 24);

  sFoot.pushSprite(0, Y_FOOT);
}

static void drawWaiting() {
  lcd.fillScreen(C(C_BG));
  lcd.setTextDatum(middle_center);
  lcd.setFont(&fonts::FreeSansBold12pt7b);
  lcd.setTextColor(C(C_CPU));
  lcd.drawString("Mac Monitor", 120, 140);
  lcd.setFont(&fonts::Font2);
  lcd.setTextColor(C(C_DIM));
  lcd.drawString("waiting for host...", 120, 170);
  lcd.drawString("run mac_agent/mac_monitor.py", 120, 190);
}

void setup() {
  Serial.begin(115200);
  // 发送永不阻塞。采集端只写不读, 设备这边的发送缓冲迟早填满, 届时 Serial.print
  // 会卡住主循环 -> 不再读串口 -> 主机写入也跟着超时, 死锁。超时设 0 就直接丢弃。
  Serial.setTxTimeoutMs(0);
  lcd.init();
  lcd.setRotation(SCREEN_ROTATION);
  lcd.setBrightness(200);
  lcd.fillScreen(C(C_BG));

  sHead.setColorDepth(16);  sHead.createSprite(240, 30);
  sGauge.setColorDepth(16); sGauge.createSprite(240, H_GAUGE);
  sCores.setColorDepth(16); sCores.createSprite(240, H_CORES);
  sUsage.setColorDepth(16); sUsage.createSprite(240, H_USE);
  sNet.setColorDepth(16);   sNet.createSprite(240, H_NET);
  sFoot.setColorDepth(16);  sFoot.createSprite(240, H_FOOT);
  Serial.printf("sprites allocated, free heap=%u\n", (unsigned)ESP.getFreeHeap());

  tkBegin("MacMonitor-Setup");
  drawWaiting();
}

// 主机断开多久后切时钟。给足重连的余量, 免得偶尔丢一帧就闪一下时钟
#define CLOCK_AFTER_MS 8000

void loop() {
  pollSerial();
  usbWatchdog();
  tkLoop();

  bool hostLive = haveData && (millis() - lastRx < CLOCK_AFTER_MS);

  // ---- 时钟模式 ----
  static bool inClock = false;
  if (!hostLive && tkHasTime()) {
    // drawClockFace 内部只重画变化的那几位, 所以可以每轮都调
    struct tm t = tkLocalTime();
    drawClockFace(lcd, t, C_BG, C_CPU, C_GPU, C_DIM, tkHasTime(), !inClock);
    inClock = true;
    delay(200);
    return;
  }

  static bool wasWaiting = true;
  if (!haveData) {
    delay(20);
    return;
  }

  // 从时钟切回仪表盘要整屏清一次, 否则大数字的残迹会留在底下
  static uint32_t lastSlow = 0;
  if (inClock || wasWaiting) {
    inClock = false;
    wasWaiting = false;
    lcd.fillScreen(C(C_BG));
    lastSlow = 0;                       // 强制立刻全量重绘
  }

  animate();

  drawGauges();
  drawCores();
  if (lastSlow == 0 || millis() - lastSlow > 400) {
    lastSlow = millis();
    drawHeader();
    drawUsage();
    drawNet();
    drawFooter();
  }
  delay(25);
}
