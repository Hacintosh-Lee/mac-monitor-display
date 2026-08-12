// DC / RST 二选一判定
//
// 已确定: SCK=GPIO2(焊盘19)  MOSI=GPIO3(焊盘20)  CS=GPIO7(焊盘23)
// 待定  : GPIO5(焊盘27) 和 GPIO4(焊盘28) 谁是 DC 谁是 RST
//
// 方案 1 蓝底大"1": DC=GPIO5, RST=GPIO4
// 方案 2 橙底大"2": DC=GPIO4, RST=GPIO5
//
// 猜错时 DC 被当成 RST 恒定拉高 -> 屏幕收到的全是 data 没有 command, 什么都不会画。
// 所以屏上最终停住的那个数字就是正确方案。

#include <Arduino.h>
#include <SPI.h>

#define TFT_W 240
#define TFT_H 320
#define SPI_HZ 40000000

#define PIN_SCK  2
#define PIN_MOSI 3
#define PIN_CS   7

static int g_dc, g_rst;
static uint16_t linebuf[TFT_W];

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
  uint8_t s = SEG7[d];
  int t = h / 8;
  int half = h / 2;
  if (s & 0x01) fillRect(x + t, y, w - 2 * t, t, col);
  if (s & 0x02) fillRect(x + w - t, y + t, t, half - t - t / 2, col);
  if (s & 0x04) fillRect(x + w - t, y + half + t / 2, t, half - t - t / 2, col);
  if (s & 0x08) fillRect(x + t, y + h - t, w - 2 * t, t, col);
  if (s & 0x10) fillRect(x, y + half + t / 2, t, half - t - t / 2, col);
  if (s & 0x20) fillRect(x, y + t, t, half - t - t / 2, col);
  if (s & 0x40) fillRect(x + t, y + half - t / 2, w - 2 * t, t, col);
}

// 现在引脚已知, 可以做真正的硬件复位(扫描固件做不到这点)
static void tryCombo(int dc, int rst, uint16_t bg, int digit) {
  g_dc = dc;
  g_rst = rst;

  SPI.end();
  pinMode(g_dc, OUTPUT);  digitalWrite(g_dc, HIGH);
  pinMode(PIN_CS, OUTPUT); digitalWrite(PIN_CS, HIGH);
  pinMode(g_rst, OUTPUT);
  digitalWrite(g_rst, LOW);   delay(20);
  digitalWrite(g_rst, HIGH);  delay(150);

  SPI.begin(PIN_SCK, -1, PIN_MOSI, -1);
  SPI.beginTransaction(SPISettings(SPI_HZ, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_CS, LOW);

  wcmd(0x01); delay(150);        // SWRESET
  wcmd(0x11); delay(150);        // SLPOUT
  wcmd(0x3A); wdat(0x55);        // 16bit
  wcmd(0x36); wdat(0x00);        // MADCTL
  wcmd(0x21);                    // INVON
  wcmd(0x13);                    // NORON
  wcmd(0x29); delay(20);         // DISPON

  fillRect(0, 0, TFT_W, TFT_H, bg);
  drawDigit(80, 100, 80, 130, digit, 0xFFFF);

  digitalWrite(PIN_CS, HIGH);
  SPI.endTransaction();
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println();
  Serial.println("=== DC/RST test: 1=蓝底(DC=GPIO5) 2=橙底(DC=GPIO4) ===");
}

void loop() {
  Serial.println("try 1: DC=GPIO5 RST=GPIO4  (blue)");
  tryCombo(5, 4, 0x001F, 1);
  delay(4000);

  Serial.println("try 2: DC=GPIO4 RST=GPIO5  (orange)");
  tryCombo(4, 5, 0xFC00, 2);
  delay(4000);
}
