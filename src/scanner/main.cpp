// ST7789 引脚探测固件 (ESP32-C3)
//
// 原理:
//   把候选 GPIO 里没被当成 SCK/MOSI/DC/CS 的那些全部输出 HIGH。
//   -> 真正的 RST 被释放(高电平), 真正的背光 BL 被点亮。
//   然后依次假设 4 个引脚的角色, 每种组合把整屏刷成一种颜色并画一个大编号。
//
//   关键点: 如果猜错了 SCK, 真正的 SCK 就是静止高电平 -> 屏幕收不到任何时钟,
//   什么都不会发生。所以只有唯一正确的组合能画上去, 而 ST7789 自带显存,
//   画上去的编号会一直 latch 在屏幕上不会被后面的错误组合擦掉。
//
//   用户只要看到屏幕上出现数字, 把数字报回来, 就能反推出确切接线。

#include <Arduino.h>
#include <SPI.h>

#define TFT_W 240
#define TFT_H 320
// 成品载板走线短, 40MHz 没问题, 扫描速度翻倍
#define SPI_HZ 40000000

// ESP32-C3 可用于接屏的 GPIO。
// 排除: 9 = BOOT 按键(板上有), 18/19 = USB D-/D+
// 20/21 是 UART0, 但这块板用 USB-Serial-JTAG 做控制台, UART 脚空着可以接屏, 所以纳入
static const uint8_t UNIV[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 20, 21};
static const int NU = sizeof(UNIV) / sizeof(UNIV[0]);

struct Combo {
  uint8_t sck, mosi, dc, cs;
};

// 先跑常见接法, 命中的话几秒内就出结果
static const Combo CURATED[] = {
    {6, 7, 3, 10}, {6, 7, 2, 10}, {6, 7, 1, 10}, {6, 7, 4, 10}, {6, 7, 5, 10},
    {6, 7, 8, 10}, {6, 7, 0, 10}, {6, 7, 10, 3}, {2, 3, 6, 7},  {2, 3, 1, 0},
    {2, 3, 4, 5},  {2, 3, 0, 1},  {4, 6, 2, 7},  {4, 5, 6, 7},  {5, 4, 6, 7},
    {1, 0, 4, 5},  {0, 1, 3, 2},  {3, 2, 1, 0},  {7, 6, 3, 10}, {8, 10, 6, 7},
    {4, 3, 2, 1},  {1, 2, 3, 4},  {5, 6, 7, 8},  {10, 7, 6, 3}, {2, 7, 6, 10},
};
static const int NCUR = sizeof(CURATED) / sizeof(CURATED[0]);

static long PERM_TOTAL;  // P(NU,4)
static long TOTAL;       // NCUR + PERM_TOTAL

static int g_sck, g_mosi, g_dc, g_cs;
static uint16_t linebuf[TFT_W];

// ---------- 底层 ----------
static inline void wcmd(uint8_t c) {
  digitalWrite(g_dc, LOW);
  SPI.transfer(c);
  digitalWrite(g_dc, HIGH);
}
static inline void wdat(uint8_t d) { SPI.transfer(d); }

static void setWin(int x, int y, int w, int h) {
  int x1 = x + w - 1, y1 = y + h - 1;
  wcmd(0x2A);
  wdat(x >> 8); wdat(x); wdat(x1 >> 8); wdat(x1);
  wcmd(0x2B);
  wdat(y >> 8); wdat(y); wdat(y1 >> 8); wdat(y1);
  wcmd(0x2C);
}

static void fillRect(int x, int y, int w, int h, uint16_t c) {
  if (w <= 0 || h <= 0 || x < 0 || y < 0) return;
  if (x + w > TFT_W) w = TFT_W - x;
  if (y + h > TFT_H) h = TFT_H - y;
  if (w <= 0 || h <= 0) return;
  uint16_t sw = (uint16_t)((c >> 8) | (c << 8));  // ST7789 要大端
  for (int i = 0; i < w; i++) linebuf[i] = sw;
  setWin(x, y, w, h);
  for (int r = 0; r < h; r++) SPI.writeBytes((const uint8_t *)linebuf, w * 2);
}

// ---------- 七段大数字 ----------
// segA..segG 位: 0=A 1=B 2=C 3=D 4=E 5=F 6=G
static const uint8_t SEG7[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66,
                                 0x6D, 0x7D, 0x07, 0x7F, 0x6F};

static void drawDigit(int x, int y, int w, int h, int d, uint16_t col) {
  if (d < 0 || d > 9) return;
  uint8_t s = SEG7[d];
  int t = h / 8;              // 段粗细
  if (t < 3) t = 3;
  int half = h / 2;
  if (s & 0x01) fillRect(x + t, y, w - 2 * t, t, col);                        // A
  if (s & 0x02) fillRect(x + w - t, y + t, t, half - t - t / 2, col);         // B
  if (s & 0x04) fillRect(x + w - t, y + half + t / 2, t, half - t - t / 2, col); // C
  if (s & 0x08) fillRect(x + t, y + h - t, w - 2 * t, t, col);                // D
  if (s & 0x10) fillRect(x, y + half + t / 2, t, half - t - t / 2, col);      // E
  if (s & 0x20) fillRect(x, y + t, t, half - t - t / 2, col);                 // F
  if (s & 0x40) fillRect(x + t, y + half - t / 2, w - 2 * t, t, col);         // G
}

static void drawNumber(long n, uint16_t col) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%ld", n);
  int len = strlen(buf);
  int dw = 46, dh = 90, gap = 12;
  int totalW = len * dw + (len - 1) * gap;
  int x0 = (TFT_W - totalW) / 2;
  int y0 = (TFT_H - dh) / 2;
  for (int i = 0; i < len; i++)
    drawDigit(x0 + i * (dw + gap), y0, dw, dh, buf[i] - '0', col);
}

// ---------- 组合枚举 ----------
static long permCount(int n, int k) {
  long r = 1;
  for (int i = 0; i < k; i++) r *= (n - i);
  return r;
}

static void getCombo(long idx, Combo *out) {
  if (idx < NCUR) {
    *out = CURATED[idx];
    return;
  }
  long n = idx - NCUR;
  bool used[16] = {false};
  uint8_t role[4];
  int avail = NU;
  for (int i = 0; i < 4; i++) {
    long rem = permCount(avail - 1, 3 - i);
    long sel = n / rem;
    n %= rem;
    int c = -1;
    for (int p = 0; p < NU; p++) {
      if (used[p]) continue;
      c++;
      if (c == sel) {
        role[i] = p;
        used[p] = true;
        break;
      }
    }
    avail--;
  }
  out->sck = UNIV[role[0]];
  out->mosi = UNIV[role[1]];
  out->dc = UNIV[role[2]];
  out->cs = UNIV[role[3]];
}

// 把不承担角色的候选脚全部拉高 -> 释放 RST, 点亮 BL
static void idleOthersHigh(const Combo &c) {
  for (int i = 0; i < NU; i++) {
    uint8_t p = UNIV[i];
    if (p == c.sck || p == c.mosi || p == c.dc || p == c.cs) continue;
    pinMode(p, OUTPUT);
    digitalWrite(p, HIGH);
  }
}

static void beginCombo(const Combo &c) {
  SPI.end();  // 先释放上一组的 SCK/MOSI, 免得和这一组的角色打架
  idleOthersHigh(c);
  g_sck = c.sck; g_mosi = c.mosi; g_dc = c.dc; g_cs = c.cs;
  pinMode(g_dc, OUTPUT);
  digitalWrite(g_dc, HIGH);
  pinMode(g_cs, OUTPUT);
  digitalWrite(g_cs, HIGH);
  SPI.begin(g_sck, -1, g_mosi, -1);
  SPI.beginTransaction(SPISettings(SPI_HZ, MSBFIRST, SPI_MODE0));
  digitalWrite(g_cs, LOW);
}

static void endCombo() {
  digitalWrite(g_cs, HIGH);
  SPI.endTransaction();
}

static const uint16_t PALETTE[] = {0x001F, 0x07E0, 0xF800, 0xFFE0,
                                   0x07FF, 0xF81F, 0xFD20, 0x8410};

// 分块状态机: 每次 loop() 只做一小段, 让 Arduino loop task 有机会喂看门狗。
// 一次性在 setup() 里跑几千次 SPI 重配置会被 WDT 打死。
enum Phase { PH_WAIT, PH_A, PH_B };
static Phase g_phase = PH_WAIT;
static long g_i = 0;
static uint32_t g_t0 = 0;

void setup() {
  Serial.begin(115200);
  // USB-Serial-JTAG 枚举要时间, 太早打印会全部丢掉(之前的崩溃就是这样看不到)
  uint32_t t = millis();
  while (!Serial && millis() - t < 4000) delay(50);
  delay(1200);

  PERM_TOTAL = permCount(NU, 4);
  TOTAL = NCUR + PERM_TOTAL;

  Serial.println();
  Serial.println("=== ST7789 pin scanner ===");
  Serial.printf("reset reason: %d\n", (int)esp_reset_reason());
  Serial.printf("candidate GPIOs: 0-8,10   combos: %ld (curated %d)\n", TOTAL, NCUR);
  Serial.flush();
  g_t0 = millis();
}

void loop() {
  switch (g_phase) {
    case PH_WAIT:
      // 开机后先静置一会, 保证串口日志一定能出来
      if (millis() - g_t0 > 1000) {
        Serial.println("phase A: waking panel (SLPOUT sweep)...");
        Serial.flush();
        g_i = 0;
        g_phase = PH_A;
      }
      break;

    case PH_A: {
      // 只发 SLPOUT, 每次 loop 处理 100 个组合
      for (int k = 0; k < 100 && g_i < TOTAL; k++, g_i++) {
        Combo c;
        getCombo(g_i, &c);
        beginCombo(c);
        wcmd(0x11);
        endCombo();
      }
      if (g_i >= TOTAL) {
        Serial.println("phase A done. phase B: scanning...");
        Serial.println(">>> 屏幕上出现数字就告诉我那个数字 <<<");
        Serial.flush();
        delay(200);
        g_i = 0;
        g_t0 = millis();
        g_phase = PH_B;
      }
      break;
    }

    case PH_B: {
      Combo c;
      getCombo(g_i, &c);
      beginCombo(c);

      wcmd(0x3A); wdat(0x55);   // 16bit color
      wcmd(0x36); wdat(0x00);   // MADCTL
      wcmd(0x21);               // INVON
      wcmd(0x13);               // NORON
      wcmd(0x29);               // DISPON
      // 只刷中间一条带子, 整屏刷太慢(全屏 61ms vs 带子 38ms), 可见度一样够
      fillRect(0, 50, TFT_W, 220, PALETTE[g_i % 8]);
      drawNumber(g_i, 0x0000);

      endCombo();

      if (g_i % 100 == 0) {
        Serial.printf("[%ld/%ld] sck=%d mosi=%d dc=%d cs=%d (%.1fs)\n", g_i,
                      TOTAL, c.sck, c.mosi, c.dc, c.cs,
                      (millis() - g_t0) / 1000.0);
      }

      g_i++;
      if (g_i >= TOTAL) {
        Serial.printf("=== pass done in %.1fs, restarting ===\n",
                      (millis() - g_t0) / 1000.0);
        g_i = 0;
        g_t0 = millis();
        delay(1500);
      }
      break;
    }
  }
}
