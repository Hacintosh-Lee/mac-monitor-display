// ============================================================
//  Mac 状态监控屏 —— 横版 320x240
//  竖版的排布直接转 90 度会很浪费横向空间, 这里改成双列:
//    左列  双环(CPU/GPU) + 温度
//    右列  内存/存储 + 网络
//    下方  核心条横贯全宽 + 底栏
//  数据由 mac_agent/mac_monitor.py 通过 USB 串口推送
// ============================================================
#include <Arduino.h>
#include "display.h"
#include "stats.h"

// ---------- 版式 (320x240, 各区严格不重叠) ----------
#define W_SCR   320
#define H_SCR   240
#define X_L     0            // 左列
#define W_L     168
#define X_R     172          // 右列
#define W_R     148

#define Y_HEAD  0
#define H_HEAD  30
#define Y_GAUGE 34
#define H_GAUGE 130
#define Y_USE   34
#define H_USE   80
#define Y_NET   118
#define H_NET   46
#define Y_CORES 170
#define H_CORES 24
#define Y_FOOT  200
#define H_FOOT  32

static LGFX lcd;
static LGFX_Sprite sHead(&lcd);
static LGFX_Sprite sGauge(&lcd);
static LGFX_Sprite sUsage(&lcd);
static LGFX_Sprite sNet(&lcd);
static LGFX_Sprite sCores(&lcd);
static LGFX_Sprite sFoot(&lcd);

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
static constexpr uint32_t C_TEMP   = 0xB8C4D4;

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
static uint16_t heat(uint32_t base, float pct) {
  if (pct < 60) return C(base);
  if (pct < 85) return lerpC(base, C_WARN, (pct - 60) / 25.0f);
  return lerpC(C_WARN, C_HOT, (pct - 85) / 15.0f);
}

// ---------- 表盘 ----------
static void drawGaugeInto(LGFX_Sprite &s, int cx, int cy, int r, float pct,
                          uint32_t base, const char *label) {
  const int th = 11;
  s.fillArc(cx, cy, r - th, r, 135, 405, C(C_TRACK));
  float sweep = 270.0f * (pct / 100.0f);
  if (sweep > 1.0f) {
    const int SEG = 24;
    uint16_t tip = heat(base, pct);
    for (int i = 0; i < SEG; i++) {
      float a0 = 135.0f + sweep * i / SEG;
      float a1 = 135.0f + sweep * (i + 1) / SEG + 0.8f;
      if (a1 > 135.0f + sweep) a1 = 135.0f + sweep;
      if (a1 <= a0) continue;
      uint16_t col = (pct < 60) ? lerpC(base & 0x3F3F3F, base, (float)i / SEG) : tip;
      s.fillArc(cx, cy, r - th, r, a0, a1, col);
    }
    if (pct >= 60) s.fillArc(cx, cy, r - th, r, 135, 135 + sweep, tip);
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
  s.drawString(label, cx, cy + r + 13);
}

// 内置字体是纯 ASCII, 没有 ° 字形; 度数符号用实心小环自己画(空心圆在 2px 半径下会断)
static void drawTemp(LGFX_Sprite &s, int cx, int cy, float t) {
  if (t <= 0.0f) return;
  char n[8];
  snprintf(n, sizeof(n), "%d", (int)(t + 0.5f));
  s.setFont(&fonts::FreeSansBold9pt7b);
  int wn = s.textWidth(n);
  int total = wn + 6 + s.textWidth("C") + 4;
  int x = cx - total / 2;
  uint16_t col = (t < 70) ? C(C_TEMP)
                          : ((t < 90) ? lerpC(C_TEMP, C_WARN, (t - 70) / 20.0f)
                                      : C(C_HOT));
  s.setTextColor(col);
  s.setTextDatum(middle_left);
  s.drawString(n, x, cy);
  int dx = x + wn + 5, dy = cy - 5;
  s.fillCircle(dx, dy, 3, col);
  s.fillCircle(dx, dy, 1, C(C_BG));
  s.drawString("C", x + wn + 10, cy);
  s.setTextDatum(middle_center);
}

static void drawGauges() {
  sGauge.fillSprite(C(C_BG));
  const int cy = 52, r = 40;
  drawGaugeInto(sGauge, 42, cy, r, an.cpu, C_CPU, "CPU");
  drawGaugeInto(sGauge, 126, cy, r, an.gpu, C_GPU, "GPU");
  drawTemp(sGauge, 42, cy + 16, an.cpuTemp);
  drawTemp(sGauge, 126, cy + 16, an.gpuTemp);
  sGauge.pushSprite(X_L, Y_GAUGE);
}

// ---------- 顶栏 ----------
static void drawFittedLabel(LGFX_Sprite &s, const char *txt, int x, int y, int maxW) {
  static const lgfx::IFont *const LADDER[] = {
      &fonts::FreeSansBold12pt7b, &fonts::FreeSansBold9pt7b,
      &fonts::FreeSans9pt7b, &fonts::Font2};
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

static void drawHeader() {
  sHead.fillSprite(C(C_BG));

  sHead.setFont(&fonts::FreeSansBold12pt7b);
  int clockW = sHead.textWidth(an.clock);
  sHead.setTextDatum(middle_right);
  sHead.setTextColor(haveData ? C(C_CPU) : C(C_DIM));
  sHead.drawString(an.clock, W_SCR - 8, 15);

  // 横版宽出 80px, 机型名基本都能用最大号字放下
  int maxW = W_SCR - 8 - clockW - 14 - 10;
  sHead.setTextDatum(middle_left);
  sHead.setTextColor(C(C_TEXT));
  sHead.setClipRect(0, 0, 10 + maxW + 1, H_HEAD - 1);
  drawFittedLabel(sHead, g_label, 10, 15, maxW);
  sHead.clearClipRect();

  for (int x = 0; x < W_SCR; x++)
    sHead.drawPixel(x, H_HEAD - 1, lerpC(C_CPU, C_GPU, x / (float)(W_SCR - 1)));

  if (millis() - lastRx > 5000) {
    sHead.setTextDatum(middle_center);
    sHead.setFont(&fonts::Font2);
    sHead.setTextColor(C(C_HOT), C(C_BG));
    sHead.drawString(" host offline ", W_SCR / 2, 15);
  }
  sHead.pushSprite(0, Y_HEAD);
}

// ---------- 内存 / 存储 ----------
// 标签用等宽字体: 比例字体下 "MEM" 的两个 M 比 "SSD" 宽, 并排看像字号不同
static void drawBar(LGFX_Sprite &s, int y, const char *label, float pct,
                    uint32_t base, const char *sub) {
  s.setTextDatum(middle_center);
  s.setFont(&fonts::FreeMonoBold9pt7b);
  s.setTextColor(C(C_DIM));
  s.drawString(label, 24, y + 6);

  int bx = 48, bw = 60;
  s.fillRoundRect(bx, y, bw, 12, 6, C(C_TRACK));
  int fw = (int)(bw * pct / 100.0f + 0.5f);
  if (fw > 4) {
    if (fw < 12) fw = 12;      // 短于一个圆头就画不出圆角
    if (fw > bw) fw = bw;
    s.fillRoundRect(bx, y, fw, 12, 6, heat(base, pct));
  }
  char p[8];
  snprintf(p, sizeof(p), "%d%%", (int)(pct + 0.5f));
  s.setTextDatum(middle_right);
  s.setFont(&fonts::Font2);
  s.setTextColor(C(C_TEXT));
  s.drawString(p, W_R - 6, y + 6);

  s.setTextDatum(middle_left);
  s.setTextColor(C(C_DIM));
  s.drawString(sub, 48, y + 24);
}

static void drawUsage() {
  sUsage.fillSprite(C(C_BG));
  char sub[32];
  snprintf(sub, sizeof(sub), "%.1f / %.1f GB", an.memUsed / 1024.0f,
           an.memTotal / 1024.0f);
  drawBar(sUsage, 8, "MEM", an.memPct, C_MEM, sub);

  if (an.diskTotal >= 1024.0f)
    snprintf(sub, sizeof(sub), "%.2f / %.2f TB", an.diskUsed / 1024.0f,
             an.diskTotal / 1024.0f);
  else
    snprintf(sub, sizeof(sub), "%.0f / %.0f GB", an.diskUsed, an.diskTotal);
  drawBar(sUsage, 48, "SSD", an.diskPct, C_DISK, sub);

  sUsage.pushSprite(X_R, Y_USE);
}

// ---------- 网络 ----------
static void arrowDown(LGFX_Sprite &s, int x, int y, uint16_t col) {
  s.fillTriangle(x, y + 5, x - 4, y - 2, x + 4, y - 2, col);
  s.fillRect(x - 1, y - 6, 3, 5, col);
}
static void arrowUp(LGFX_Sprite &s, int x, int y, uint16_t col) {
  s.fillTriangle(x, y - 6, x - 4, y + 1, x + 4, y + 1, col);
  s.fillRect(x - 1, y + 1, 3, 5, col);
}

// 右列只有 148px, 放两个 "1.2 MB/s" 这样的完整写法必然打架, 压缩成 "1.2M/s"。
// 箭头已经表明了方向, 单位省掉 B 也不会有歧义。
static void fmtRateShort(float kbs, char *out, size_t n) {
  if (kbs >= 1024.0f) snprintf(out, n, "%.1fM/s", kbs / 1024.0f);
  else if (kbs >= 100.0f) snprintf(out, n, "%.0fK/s", kbs);
  else snprintf(out, n, "%.1fK/s", kbs);
}

static void drawNet() {
  sNet.fillSprite(C(C_BG));
  char r[16], t[16];
  fmtRateShort(an.rx, r, sizeof(r));
  fmtRateShort(an.tx, t, sizeof(t));

  sNet.setFont(&fonts::Font2);
  sNet.setTextDatum(middle_left);
  sNet.setTextColor(C(C_TEXT));

  // 下行靠左
  arrowDown(sNet, 10, 9, C(C_RX));
  sNet.drawString(r, 18, 9);
  int downEnd = 18 + sNet.textWidth(r);

  // 上行靠右: 箭头位置由文字宽度从右边界倒推。必须夹住下界 —— 之前没夹, 速率一变长
  // 倒推出来的位置就会越过去压在下行数字上。
  int wt = sNet.textWidth(t);
  int upX = W_R - 6 - wt - 8;
  if (upX < downEnd + 12) upX = downEnd + 12;
  arrowUp(sNet, upX, 9, C(C_TX));
  sNet.setClipRect(0, 0, W_R, 18);   // 极端长度下宁可截断, 也不许溢出画到别处
  sNet.drawString(t, upX + 8, 9);
  sNet.clearClipRect();

  int y0 = 20, h = 24;
  sNet.fillRoundRect(2, y0, W_R - 4, h, 3, C(C_CARD));
  int step = (W_R - 10) / HIST;
  if (step < 1) step = 1;
  for (int i = 0; i < HIST; i++) {
    int v = hNet[(hPos + i) % HIST];
    int bh = v * (h - 4) / 100;
    if (bh > 0)
      sNet.fillRect(4 + i * step, y0 + h - 2 - bh, step > 1 ? step - 1 : 1, bh,
                    lerpC(C_RX, C_TX, v / 100.0f));
  }
  sNet.pushSprite(X_R, Y_NET);
}

// ---------- 核心条 (横贯全宽, 横版最舒服的地方) ----------
static void drawCores() {
  sCores.fillSprite(C(C_BG));
  int n = st.ncore > 0 ? st.ncore : 10;
  if (n > MAXCORE) n = MAXCORE;
  int gap = n > 32 ? 1 : (n > 16 ? 2 : 3);
  int avail = W_SCR - 16;
  int w = (avail - gap * (n - 1)) / n;
  if (w < 2) w = 2;
  int x = (W_SCR - (w * n + gap * (n - 1))) / 2;
  const int bh = H_CORES - 2;
  for (int i = 0; i < n; i++) {
    float v = an.core[i];
    int h = (int)(bh * v / 100.0f + 0.5f);
    int rr = w > 4 ? 2 : 1;   // 柱子窄到 2px 时圆角比柱子还宽, 画不出形状
    sCores.fillRoundRect(x, 0, w, bh, rr, C(C_TRACK));
    if (h > 0) sCores.fillRoundRect(x, bh - h, w, h, rr, heat(C_CPU, v));
    x += w + gap;
  }
  sCores.pushSprite(0, Y_CORES);
}

// ---------- 底栏 (横版够宽, 一行排完) ----------
static void drawFoot() {
  sFoot.fillSprite(C(C_BG));
  char s[48];
  sFoot.setFont(&fonts::Font2);
  sFoot.setTextDatum(middle_left);

  sFoot.setTextColor(C(C_DIM));
  sFoot.drawString("LOAD", 10, 15);
  snprintf(s, sizeof(s), "%.2f", an.load1);
  sFoot.setTextColor(heat(C_CPU, an.load1 * 10.0f));
  sFoot.drawString(s, 48, 15);

  sFoot.setTextColor(C(C_DIM));
  sFoot.drawString("UP", 100, 15);
  long h = st.upMin / 60, m = st.upMin % 60;
  if (h >= 24) snprintf(s, sizeof(s), "%ldd %ldh", h / 24, h % 24);
  else snprintf(s, sizeof(s), "%ldh %ldm", h, m);
  sFoot.setTextColor(C(C_TEXT));
  sFoot.drawString(s, 124, 15);

  snprintf(s, sizeof(s), "%d%%", st.topCpu);
  sFoot.setTextDatum(middle_right);
  sFoot.setTextColor(C(C_DIM));
  sFoot.drawString(s, W_SCR - 10, 15);
  int pw = sFoot.textWidth(s);
  sFoot.setTextColor(C(C_GPU));
  sFoot.drawString(st.topName, W_SCR - 16 - pw, 15);

  sFoot.pushSprite(0, Y_FOOT);
}

static void drawWaiting() {
  lcd.fillScreen(C(C_BG));
  lcd.setTextDatum(middle_center);
  lcd.setFont(&fonts::FreeSansBold18pt7b);
  lcd.setTextColor(C(C_CPU));
  lcd.drawString("Mac Monitor", W_SCR / 2, 100);
  lcd.setFont(&fonts::Font2);
  lcd.setTextColor(C(C_DIM));
  lcd.drawString("waiting for host...", W_SCR / 2, 134);
  lcd.drawString("run mac_agent/mac_monitor.py", W_SCR / 2, 154);
}

void setup() {
  Serial.begin(115200);
  lcd.init();
  lcd.setRotation(1);          // 横版: 面板本身是 240x320, 转 90 度成 320x240
  lcd.setBrightness(200);
  lcd.fillScreen(C(C_BG));

  // 逐个检查分配结果。横版的表盘精灵有 43KB, 比竖版任何一块都大, 堆碎片化时
  // 最先失败的就是它 —— 失败后 LovyanGFX 会静默不画, 屏幕一片黑很难判断原因。
  struct { LGFX_Sprite *s; int w, h; const char *name; } SP[] = {
      {&sHead, W_SCR, H_HEAD, "head"},   {&sGauge, W_L, H_GAUGE, "gauge"},
      {&sUsage, W_R, H_USE, "usage"},    {&sNet, W_R, H_NET, "net"},
      {&sCores, W_SCR, H_CORES, "cores"},{&sFoot, W_SCR, H_FOOT, "foot"},
  };
  Serial.printf("heap before sprites = %u, largest block = %u\n",
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
  for (auto &e : SP) {
    e.s->setColorDepth(16);
    if (!e.s->createSprite(e.w, e.h))
      Serial.printf("!! sprite '%s' %dx%d (%d bytes) 分配失败, 剩余 %u\n", e.name,
                    e.w, e.h, e.w * e.h * 2, (unsigned)ESP.getFreeHeap());
  }
  Serial.printf("landscape up, free heap=%u\n", (unsigned)ESP.getFreeHeap());

  drawWaiting();
}

void loop() {
  pollSerial();

  static bool wasWaiting = true;
  if (!haveData) {
    delay(20);
    return;
  }
  if (wasWaiting) {
    lcd.fillScreen(C(C_BG));
    wasWaiting = false;
  }

  animate();

  static uint32_t lastSlow = 0;
  drawGauges();
  drawCores();
  if (millis() - lastSlow > 400) {
    lastSlow = millis();
    drawHeader();
    drawUsage();
    drawNet();
    drawFoot();
  }
  delay(25);
}
