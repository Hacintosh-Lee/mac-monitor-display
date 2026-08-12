// 聚焦扫描: 已知屏只用 GPIO 2/3/4/5/7 这 5 个脚, 穷举它们的角色分配。
//
// 覆盖:
//   block1  sck=2,mosi=3 固定(丝印 SPI_CK/SPI_MOSI), {4,5,7} 排成 DC/CS/RST  -> 12  (快速通道)
//   block2  5 个脚到 5 个角色的全双射                                        -> 240
//   block3  只用 SCK/MOSI/DC, 其余恒高 (应对 CS 在板上直接拉低的情况)         -> 120
//   以上每种都跑 SPI_MODE0 和 SPI_MODE3
//
// 命中的那一组会在屏上画出彩色横带 + 大编号并 latch 住。把编号报回来即可反查。

#include <Arduino.h>
#include <SPI.h>

#define TFT_W 240
#define TFT_H 320
#define BAND_Y 60
#define BAND_H 150
#define SPI_HZ 20000000

static const uint8_t P5[5] = {2, 3, 4, 5, 7};

struct Cfg {
  uint8_t sck, mosi, dc, cs, rst;
  uint8_t useCS, useRST, mode;
};
static Cfg TBL[400];
static int NTBL = 0;

static int g_dc, g_cs;
static bool g_useCS;
static uint16_t linebuf[TFT_W];

static void buildTable() {
  NTBL = 0;
  // block1: 丝印顺理成章的那 6 种, 放最前面当快速通道
  static const uint8_t tri[6][3] = {{4, 5, 7}, {4, 7, 5}, {5, 4, 7},
                                    {5, 7, 4}, {7, 4, 5}, {7, 5, 4}};
  for (int m = 0; m < 2; m++)
    for (int p = 0; p < 6; p++)
      TBL[NTBL++] = {2, 3, tri[p][0], tri[p][1], tri[p][2], 1, 1,
                     (uint8_t)(m ? 3 : 0)};

  // block2: 全双射 5! = 120
  for (int m = 0; m < 2; m++)
    for (int a = 0; a < 5; a++)
      for (int b = 0; b < 5; b++) {
        if (b == a) continue;
        for (int c = 0; c < 5; c++) {
          if (c == a || c == b) continue;
          for (int d = 0; d < 5; d++) {
            if (d == a || d == b || d == c) continue;
            for (int e = 0; e < 5; e++) {
              if (e == a || e == b || e == c || e == d) continue;
              TBL[NTBL++] = {P5[a], P5[b], P5[c], P5[d], P5[e], 1, 1,
                             (uint8_t)(m ? 3 : 0)};
            }
          }
        }
      }

  // block3: CS 可能在板上直接接地, 只驱动 SCK/MOSI/DC, 其余恒高
  for (int m = 0; m < 2; m++)
    for (int a = 0; a < 5; a++)
      for (int b = 0; b < 5; b++) {
        if (b == a) continue;
        for (int c = 0; c < 5; c++) {
          if (c == a || c == b) continue;
          TBL[NTBL++] = {P5[a], P5[b], P5[c], 0, 0, 0, 0, (uint8_t)(m ? 3 : 0)};
        }
      }
}

static inline void wcmd(uint8_t c) {
  digitalWrite(g_dc, LOW);
  SPI.transfer(c);
  digitalWrite(g_dc, HIGH);
}
static inline void wdat(uint8_t d) { SPI.transfer(d); }

static void setWin(int x, int y, int w, int h) {
  int x1 = x + w - 1, y1 = y + h - 1;
  wcmd(0x2A); wdat(x >> 8); wdat(x); wdat(x1 >> 8); wdat(x1);
  wcmd(0x2B); wdat(y >> 8); wdat(y); wdat(y1 >> 8); wdat(y1);
  wcmd(0x2C);
}

static void fillRect(int x, int y, int w, int h, uint16_t c) {
  if (w <= 0 || h <= 0) return;
  uint16_t sw = (uint16_t)((c >> 8) | (c << 8));
  for (int i = 0; i < w; i++) linebuf[i] = sw;
  setWin(x, y, w, h);
  for (int r = 0; r < h; r++) SPI.writeBytes((const uint8_t *)linebuf, w * 2);
}

static const uint8_t SEG7[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66,
                                 0x6D, 0x7D, 0x07, 0x7F, 0x6F};

static void drawDigit(int x, int y, int w, int h, int d, uint16_t col) {
  if (d < 0 || d > 9) return;
  uint8_t s = SEG7[d];
  int t = h / 8, half = h / 2;
  if (s & 0x01) fillRect(x + t, y, w - 2 * t, t, col);
  if (s & 0x02) fillRect(x + w - t, y + t, t, half - t - t / 2, col);
  if (s & 0x04) fillRect(x + w - t, y + half + t / 2, t, half - t - t / 2, col);
  if (s & 0x08) fillRect(x + t, y + h - t, w - 2 * t, t, col);
  if (s & 0x10) fillRect(x, y + half + t / 2, t, half - t - t / 2, col);
  if (s & 0x20) fillRect(x, y + t, t, half - t - t / 2, col);
  if (s & 0x40) fillRect(x + t, y + half - t / 2, w - 2 * t, t, col);
}

static void drawNumber(int n, uint16_t col) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%d", n);
  int len = strlen(buf);
  int dw = 44, dh = 110, gap = 10;
  int x0 = (TFT_W - (len * dw + (len - 1) * gap)) / 2;
  int y0 = BAND_Y + (BAND_H - dh) / 2;
  for (int i = 0; i < len; i++)
    drawDigit(x0 + i * (dw + gap), y0, dw, dh, buf[i] - '0', col);
}

static const uint16_t PAL[8] = {0x001F, 0x07E0, 0xF800, 0xFFE0,
                                0x07FF, 0xF81F, 0xFD20, 0xFFFF};

static void runCfg(const Cfg &c, int idx) {
  SPI.end();

  // 没参与的脚一律恒高: 释放可能存在的 RST, 也不会误拉低 CS
  for (int i = 0; i < 5; i++) {
    uint8_t p = P5[i];
    if (p == c.sck || p == c.mosi || p == c.dc) continue;
    if (c.useCS && p == c.cs) continue;
    if (c.useRST && p == c.rst) continue;
    pinMode(p, OUTPUT);
    digitalWrite(p, HIGH);
  }

  g_dc = c.dc;
  g_useCS = c.useCS;
  g_cs = c.useCS ? c.cs : -1;

  pinMode(g_dc, OUTPUT);
  digitalWrite(g_dc, HIGH);
  if (g_useCS) {
    pinMode(g_cs, OUTPUT);
    digitalWrite(g_cs, HIGH);
  }

  // 引脚已知范围很小, 这里做真正的硬件复位, 比扫描固件靠谱
  if (c.useRST) {
    pinMode(c.rst, OUTPUT);
    digitalWrite(c.rst, LOW);
    delay(10);
    digitalWrite(c.rst, HIGH);
    delay(50);
  }

  SPI.begin(c.sck, -1, c.mosi, -1);
  SPI.beginTransaction(SPISettings(SPI_HZ, MSBFIRST, c.mode));
  if (g_useCS) digitalWrite(g_cs, LOW);

  wcmd(0x11); delay(100);   // SLPOUT
  wcmd(0x3A); wdat(0x55);   // 16bit/px
  wcmd(0x36); wdat(0x00);   // MADCTL
  wcmd(0x21);               // INVON
  wcmd(0x13);               // NORON
  wcmd(0x29);               // DISPON

  fillRect(0, BAND_Y, TFT_W, BAND_H, PAL[idx % 8]);
  drawNumber(idx, 0x0000);

  if (g_useCS) digitalWrite(g_cs, HIGH);
  SPI.endTransaction();
}

void setup() {
  Serial.begin(115200);
  uint32_t t = millis();
  while (!Serial && millis() - t < 3000) delay(50);
  delay(1000);
  buildTable();
  Serial.printf("\n=== focus scan: %d combos over GPIO 2/3/4/5/7 ===\n", NTBL);
  Serial.flush();
}

void loop() {
  static int i = 0;
  static uint32_t t0 = millis();

  runCfg(TBL[i], i);

  if (i % 25 == 0) {
    const Cfg &c = TBL[i];
    Serial.printf("[%d/%d] sck=%d mosi=%d dc=%d cs=%d rst=%d mode=%d (%.1fs)\n",
                  i, NTBL, c.sck, c.mosi, c.dc, c.useCS ? c.cs : -1,
                  c.useRST ? c.rst : -1, c.mode, (millis() - t0) / 1000.0);
  }

  if (++i >= NTBL) {
    Serial.printf("=== pass done in %.1fs ===\n", (millis() - t0) / 1000.0);
    i = 0;
    t0 = millis();
    delay(1200);
  }
}
