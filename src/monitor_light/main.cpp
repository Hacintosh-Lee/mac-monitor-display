// ============================================================
//  Mac 状态监控屏 —— 浅色 Apple 风
//  卡片式分组 + 圆头进度环, 配色取 iOS 系统色
//  数据由 mac_agent/mac_monitor.py 通过 USB 串口推送
// ============================================================
#include <Arduino.h>
#include "display.h"
#include "stats.h"

static LGFX lcd;
static LGFX_Sprite sRings(&lcd);  // 240x108 双环卡片
static LGFX_Sprite sCores(&lcd);  // 240x34  核心
static LGFX_Sprite sUse(&lcd);    // 240x60  内存/存储
static LGFX_Sprite sNet(&lcd);    // 240x46  网络
static LGFX_Sprite sFoot(&lcd);   // 240x20  底栏

// ---------- iOS 系统配色 ----------
static constexpr uint32_t C_BG     = 0xF2F2F7;  // systemGroupedBackground
static constexpr uint32_t C_CARD   = 0xFFFFFF;
static constexpr uint32_t C_HAIR   = 0xE5E5EA;  // separator
static constexpr uint32_t C_TRACK  = 0xE9E9EB;  // 进度槽
static constexpr uint32_t C_LABEL  = 0x1C1C1E;  // label
static constexpr uint32_t C_LABEL2 = 0x8E8E93;  // secondaryLabel
static constexpr uint32_t C_BLUE   = 0x007AFF;
static constexpr uint32_t C_PURPLE = 0xAF52DE;
static constexpr uint32_t C_ORANGE = 0xFF9500;
static constexpr uint32_t C_GREEN  = 0x34C759;
static constexpr uint32_t C_CYAN   = 0x32ADE6;
static constexpr uint32_t C_PINK   = 0xFF2D55;
static constexpr uint32_t C_RED    = 0xFF3B30;

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
// 负载高时向橙->红过渡, 但保持 iOS 系统色的调子
static uint16_t heat(uint32_t base, float pct) {
  if (pct < 65) return C(base);
  if (pct < 88) return lerpC(base, C_ORANGE, (pct - 65) / 23.0f);
  return lerpC(C_ORANGE, C_RED, (pct - 88) / 12.0f);
}

// ---------- 版式 ----------
#define CARD_R 14
#define MARGIN 10
#define Y_RINGS 38
#define Y_CORES 150
#define Y_USE   188
#define Y_NET   252
#define Y_FOOT  300

static void card(LGFX_Sprite &s, int x, int y, int w, int h) {
  s.fillRoundRect(x, y, w, h, CARD_R, C(C_CARD));
  s.drawRoundRect(x, y, w, h, CARD_R, C(C_HAIR));
}

// Apple 的圆环两端是圆头的, fillArc 画出来是平口, 得自己在两端补圆点
static void ringArc(LGFX_Sprite &s, int cx, int cy, int r0, int r1, float a0,
                    float a1, uint16_t col) {
  if (a1 <= a0) return;
  s.fillArc(cx, cy, r0, r1, a0, a1, col);
  int rm = (r0 + r1) / 2, cap = (r1 - r0) / 2;
  float ra0 = a0 * DEG_TO_RAD, ra1 = a1 * DEG_TO_RAD;
  s.fillCircle(cx + cosf(ra0) * rm, cy + sinf(ra0) * rm, cap, col);
  s.fillCircle(cx + cosf(ra1) * rm, cy + sinf(ra1) * rm, cap, col);
}

static void drawRing(LGFX_Sprite &s, int cx, int cy, float pct, uint32_t base,
                     const char *label) {
  const int r1 = 34, r0 = r1 - 9;
  ringArc(s, cx, cy, r0, r1, 135, 405, C(C_TRACK));
  float sweep = 270.0f * (pct / 100.0f);
  if (sweep > 2.0f) ringArc(s, cx, cy, r0, r1, 135, 135 + sweep, heat(base, pct));

  char num[8];
  snprintf(num, sizeof(num), "%d", (int)(pct + 0.5f));
  s.setTextDatum(middle_center);
  s.setFont(&fonts::FreeSansBold12pt7b);
  s.setTextColor(C(C_LABEL));
  s.drawString(num, cx, cy - 2);
  s.setFont(&fonts::Font2);
  s.setTextColor(C(C_LABEL2));
  s.drawString("%", cx, cy + 16);

  s.setFont(&fonts::FreeSansBold9pt7b);
  s.setTextColor(heat(base, pct));
  s.drawString(label, cx, cy + 44);
}

// 机型名长度差异很大, 按字体阶梯降级适配剩余宽度, 短名字保持大字号
static void drawFittedLabel(const char *s, int x, int y, int maxW) {
  static const lgfx::IFont *const LADDER[] = {
      &fonts::FreeSansBold9pt7b, &fonts::FreeSans9pt7b, &fonts::Font2};
  for (auto f : LADDER) {
    lcd.setFont(f);
    if (lcd.textWidth(s) <= maxW) {
      lcd.drawString(s, x, y);
      return;
    }
  }
  lcd.setFont(&fonts::Font2);
  char buf[40];
  size_t n = strlen(s);
  if (n > sizeof(buf) - 4) n = sizeof(buf) - 4;
  while (n > 1) {
    memcpy(buf, s, n);
    strcpy(buf + n, "...");
    if (lcd.textWidth(buf) <= maxW) break;
    n--;
  }
  lcd.drawString(buf, x, y);
}

static void drawHeader() {
  lcd.fillRect(0, 0, 240, 36, C(C_BG));

  lcd.setFont(&fonts::FreeSansBold12pt7b);
  int clockW = lcd.textWidth(an.clock);
  lcd.setTextDatum(middle_right);
  lcd.setTextColor(C(C_LABEL));
  lcd.drawString(an.clock, 230, 18);

  // 左侧: 连接状态小圆点 + 机型名
  bool live = (millis() - lastRx) < 5000;
  lcd.fillCircle(16, 18, 4, live ? C(C_GREEN) : C(C_LABEL2));

  int x0 = 26;
  int maxW = 230 - clockW - 14 - x0;
  lcd.setTextDatum(middle_left);
  lcd.setTextColor(C(C_LABEL));
  lcd.setClipRect(0, 0, x0 + maxW + 1, 36);
  drawFittedLabel(g_label, x0, 18, maxW);
  lcd.clearClipRect();
}

static void drawRings() {
  sRings.fillSprite(C(C_BG));
  card(sRings, MARGIN, 0, 105, 108);
  card(sRings, 125, 0, 105, 108);
  drawRing(sRings, MARGIN + 52, 46, an.cpu, C_BLUE, "CPU");
  drawRing(sRings, 125 + 52, 46, an.gpu, C_PURPLE, "GPU");
  sRings.pushSprite(0, Y_RINGS);
}

static void drawCores() {
  sCores.fillSprite(C(C_BG));
  card(sCores, MARGIN, 0, 220, 34);
  int n = st.ncore > 0 ? st.ncore : 10;
  if (n > MAXCORE) n = MAXCORE;
  int gap = n > 32 ? 1 : (n > 16 ? 2 : 3);
  int avail = 220 - 24;
  int w = (avail - gap * (n - 1)) / n;
  if (w < 2) w = 2;
  int x = MARGIN + 12 + (avail - (w * n + gap * (n - 1))) / 2;
  for (int i = 0; i < n; i++) {
    float v = an.core[i];
    int h = (int)(18.0f * v / 100.0f + 0.5f);
    sCores.fillRoundRect(x, 8, w, 18, w > 4 ? 2 : 1, C(C_TRACK));
    if (h > 1) sCores.fillRoundRect(x, 8 + 18 - h, w, h, w > 4 ? 2 : 1, heat(C_BLUE, v));
    x += w + gap;
  }
  sCores.pushSprite(0, Y_CORES);
}

static void drawRow(LGFX_Sprite &s, int y, const char *label, float pct,
                    uint32_t base, const char *val) {
  s.setFont(&fonts::FreeSansBold9pt7b);
  s.setTextDatum(middle_left);
  s.setTextColor(C(C_LABEL));
  s.drawString(label, MARGIN + 14, y);

  s.setFont(&fonts::Font2);
  s.setTextDatum(middle_right);
  s.setTextColor(C(C_LABEL2));
  s.drawString(val, 230 - 14, y);

  int bx = MARGIN + 14, bw = 220 - 28, by = y + 12;
  s.fillRoundRect(bx, by, bw, 6, 3, C(C_TRACK));
  int fw = (int)(bw * pct / 100.0f + 0.5f);
  if (fw > 6) s.fillRoundRect(bx, by, fw, 6, 3, heat(base, pct));
}

static void drawUse() {
  sUse.fillSprite(C(C_BG));
  card(sUse, MARGIN, 0, 220, 60);
  char v[32];
  snprintf(v, sizeof(v), "%.1f / %.0f GB", an.memUsed / 1024.0f, an.memTotal / 1024.0f);
  drawRow(sUse, 14, "Memory", an.memPct, C_ORANGE, v);

  if (an.diskTotal >= 1024.0f)
    snprintf(v, sizeof(v), "%.2f / %.2f TB", an.diskUsed / 1024.0f, an.diskTotal / 1024.0f);
  else
    snprintf(v, sizeof(v), "%.0f / %.0f GB", an.diskUsed, an.diskTotal);
  drawRow(sUse, 40, "Storage", an.diskPct, C_GREEN, v);
  sUse.pushSprite(0, Y_USE);
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
  card(sNet, MARGIN, 0, 220, 46);

  char r[16], t[16];
  fmtRate(an.rx, r, sizeof(r));
  fmtRate(an.tx, t, sizeof(t));

  sNet.setFont(&fonts::FreeSansBold9pt7b);
  sNet.setTextDatum(middle_left);
  arrowDown(sNet, MARGIN + 18, 14, C(C_CYAN));
  sNet.setTextColor(C(C_LABEL));
  sNet.drawString(r, MARGIN + 28, 14);

  sNet.setTextDatum(middle_right);
  sNet.drawString(t, 230 - 14, 14);
  arrowUp(sNet, 230 - 18 - sNet.textWidth(t) - 6, 14, C(C_PINK));

  // 波形
  int y0 = 27, h = 13, x0 = MARGIN + 14;
  int bw = 220 - 28;
  int step = bw / HIST;
  if (step < 1) step = 1;
  for (int i = 0; i < HIST; i++) {
    int v = hNet[(hPos + i) % HIST];
    int bh = v * h / 100;
    if (bh < 1 && v > 0) bh = 1;
    if (bh > 0)
      sNet.fillRect(x0 + i * step, y0 + h - bh, step > 1 ? step - 1 : 1, bh,
                    lerpC(C_CYAN, C_PINK, v / 100.0f));
  }
  sNet.pushSprite(0, Y_NET);
}

static void drawFoot() {
  sFoot.fillSprite(C(C_BG));
  char s[48];
  sFoot.setFont(&fonts::Font2);
  sFoot.setTextDatum(middle_left);

  sFoot.setTextColor(C(C_LABEL2));
  long h = st.upMin / 60, m = st.upMin % 60;
  if (h >= 24) snprintf(s, sizeof(s), "Load %.2f   Up %ldd %ldh", an.load1, h / 24, h % 24);
  else snprintf(s, sizeof(s), "Load %.2f   Up %ldh %ldm", an.load1, h, m);
  sFoot.drawString(s, MARGIN + 4, 10);

  snprintf(s, sizeof(s), "%s %d%%", st.topName, st.topCpu);
  sFoot.setTextDatum(middle_right);
  sFoot.setTextColor(C(C_PURPLE));
  sFoot.drawString(s, 230, 10);
  sFoot.pushSprite(0, Y_FOOT);
}

static void drawWaiting() {
  lcd.fillScreen(C(C_BG));
  lcd.fillRoundRect(MARGIN, 120, 220, 90, CARD_R, C(C_CARD));
  lcd.drawRoundRect(MARGIN, 120, 220, 90, CARD_R, C(C_HAIR));
  lcd.setTextDatum(middle_center);
  lcd.setFont(&fonts::FreeSansBold12pt7b);
  lcd.setTextColor(C(C_LABEL));
  lcd.drawString("Mac Monitor", 120, 150);
  lcd.setFont(&fonts::Font2);
  lcd.setTextColor(C(C_LABEL2));
  lcd.drawString("waiting for host", 120, 175);
}

void setup() {
  Serial.begin(115200);
  lcd.init();
  lcd.setRotation(0);
  lcd.fillScreen(C(C_BG));

  sRings.setColorDepth(16); sRings.createSprite(240, 108);
  sCores.setColorDepth(16); sCores.createSprite(240, 34);
  sUse.setColorDepth(16);   sUse.createSprite(240, 60);
  sNet.setColorDepth(16);   sNet.createSprite(240, 46);
  sFoot.setColorDepth(16);  sFoot.createSprite(240, 20);
  Serial.printf("light theme up, free heap=%u\n", (unsigned)ESP.getFreeHeap());

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
  drawRings();
  drawCores();
  if (millis() - lastSlow > 400) {
    lastSlow = millis();
    drawHeader();
    drawUse();
    drawNet();
    drawFoot();
  }
  delay(25);
}
