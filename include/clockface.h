// 大字时钟, 参考 iOS StandBy: 巨大渐变数字 + 角落日期。
//
// 数字不是用字体画的。位图字体放大 3 倍锯齿很明显, 而嵌入一套漂亮的大号字体
// 在公开仓库有授权问题。这里把每个数字定义成"线段 + 圆弧"的路径, 沿路径盖圆点
// 描边 —— 圆角笔帽和真曲线都有了, 任意尺寸都清晰。
//
// 渐变也顺带解决了: 每个圆点按自己所在的 y 取色, 一遍画完, 不需要额外的图层或精灵。
#pragma once
#include <Arduino.h>
#include <math.h>

// 路径段: LINE(x1,y1,x2,y2) 或 ARC(cx,cy,rx,ry,起始角,终止角), 坐标都是 0..1 归一化
struct CSeg { uint8_t type; float a, b, c, d, e, f; };
#define CL(x1, y1, x2, y2)        {0, x1, y1, x2, y2, 0, 0}
#define CA(cx, cy, rx, ry, s, e)  {1, cx, cy, rx, ry, s, e}

// 每个数字最多 4 段
static const CSeg CDIG[10][4] = {
  // 角度: 0=右 90=下 180=左 270=上; a1<a0 为逆时针。
  //
  // 关键: 相邻段必须共享端点。之前 9 的圆和尾巴、3 的上下两碗都是各画各的,
  // 端点差了一大截, 于是看起来是断开的两截 —— 加粗只能盖住一部分, 治标不治本。
  // 下面每处衔接的坐标都是从上一段的终点反算出来的。

  // 0  单个椭圆
  {CA(0.50f, 0.50f, 0.32f, 0.42f, 0, 360), CL(0,0,0,0), CL(0,0,0,0), CL(0,0,0,0)},
  // 1  竖笔 + 左上小旗
  {CL(0.54f, 0.07f, 0.54f, 0.93f), CL(0.54f, 0.09f, 0.25f, 0.28f), CL(0,0,0,0), CL(0,0,0,0)},
  // 2  上碗(左->上->右, 终点 0.79/0.36) -> 斜线 -> 底横
  {CA(0.50f, 0.30f, 0.30f, 0.22f, 180, 375), CL(0.79f, 0.36f, 0.14f, 0.90f),
   CL(0.14f, 0.90f, 0.88f, 0.90f), CL(0,0,0,0)},
  // 3  上碗终点(0.48,0.52) 与下碗起点(0.48,0.52) 重合
  {CA(0.48f, 0.30f, 0.28f, 0.22f, 180, 450), CA(0.48f, 0.74f, 0.32f, 0.22f, 270, 540),
   CL(0,0,0,0), CL(0,0,0,0)},
  // 4  竖笔从顶点下来, 斜线与横线共用 (0.09,0.63)
  {CL(0.68f, 0.08f, 0.09f, 0.63f), CL(0.09f, 0.63f, 0.92f, 0.63f),
   CL(0.68f, 0.08f, 0.68f, 0.92f), CL(0,0,0,0)},
  // 5  顶横 -> 左竖(终点 0.20/0.50) -> 碗从 215 度起, 正好接上
  {CL(0.82f, 0.09f, 0.22f, 0.09f), CL(0.22f, 0.09f, 0.20f, 0.50f),
   CA(0.48f, 0.66f, 0.30f, 0.27f, 215, 505), CL(0,0,0,0)},
  // 6  上弯钩(起点 0.25/0.31) -> 左竖 -> 下圆(左端 0.19/0.70)
  {CA(0.52f, 0.31f, 0.27f, 0.23f, 180, 345), CL(0.25f, 0.31f, 0.19f, 0.68f),
   CA(0.50f, 0.70f, 0.31f, 0.23f, 0, 360), CL(0,0,0,0)},
  // 7  顶横 + 长斜线, 共用 (0.89,0.09)
  {CL(0.11f, 0.09f, 0.89f, 0.09f), CL(0.89f, 0.09f, 0.36f, 0.93f), CL(0,0,0,0), CL(0,0,0,0)},
  // 8  上下两椭圆在 0.51 附近交叠
  {CA(0.50f, 0.30f, 0.27f, 0.22f, 0, 360), CA(0.50f, 0.72f, 0.31f, 0.22f, 0, 360),
   CL(0,0,0,0), CL(0,0,0,0)},
  // 9  上圆右端(0.77,0.30) -> 竖笔 -> 下弯钩(起点 0.79/0.66)
  {CA(0.48f, 0.30f, 0.29f, 0.23f, 0, 360), CL(0.77f, 0.30f, 0.79f, 0.66f),
   CA(0.48f, 0.66f, 0.31f, 0.24f, 0, 125), CL(0,0,0,0)},
};
static const uint8_t CDIG_N[10] = {1, 2, 3, 2, 3, 3, 3, 2, 2, 3};

// 竖向渐变取色
static inline uint16_t clockGrad(LGFX_Device &lcd, uint32_t c0, uint32_t c1,
                                 int y, int y0, int y1) {
  float t = (y1 > y0) ? (float)(y - y0) / (y1 - y0) : 0.0f;
  if (t < 0) t = 0;
  if (t > 1) t = 1;
  int r = ((c0 >> 16) & 0xFF) + (((int)((c1 >> 16) & 0xFF) - (int)((c0 >> 16) & 0xFF)) * t);
  int g = ((c0 >> 8) & 0xFF) + (((int)((c1 >> 8) & 0xFF) - (int)((c0 >> 8) & 0xFF)) * t);
  int b = (c0 & 0xFF) + (((int)(c1 & 0xFF) - (int)(c0 & 0xFF)) * t);
  return lcd.color565(r, g, b);
}

// 沿路径盖圆点描边。步长按设备像素算, 保证不同尺寸下笔画都连续不断
static void clockStroke(LGFX_Device &lcd, const CSeg &s, int x, int y, int w, int h,
                        int th, uint32_t c0, uint32_t c1, int gy0, int gy1) {
  int rad = th / 2;
  if (s.type == 0) {
    float x1 = x + s.a * w, y1 = y + s.b * h;
    float x2 = x + s.c * w, y2 = y + s.d * h;
    float len = sqrtf((x2 - x1) * (x2 - x1) + (y2 - y1) * (y2 - y1));
    int n = (int)(len / 1.5f) + 1;
    for (int i = 0; i <= n; i++) {
      float t = (float)i / n;
      int px = (int)(x1 + (x2 - x1) * t), py = (int)(y1 + (y2 - y1) * t);
      lcd.fillCircle(px, py, rad, clockGrad(lcd, c0, c1, py, gy0, gy1));
    }
  } else {
    float cx = x + s.a * w, cy = y + s.b * h;
    float rx = s.c * w, ry = s.d * h;
    float a0 = s.e * DEG_TO_RAD, a1 = s.f * DEG_TO_RAD;
    float arclen = fabsf(a1 - a0) * (rx + ry) * 0.5f;
    int n = (int)(arclen / 1.5f) + 1;
    for (int i = 0; i <= n; i++) {
      float a = a0 + (a1 - a0) * i / n;
      int px = (int)(cx + cosf(a) * rx), py = (int)(cy + sinf(a) * ry);
      lcd.fillCircle(px, py, rad, clockGrad(lcd, c0, c1, py, gy0, gy1));
    }
  }
}

static void clockDigit(LGFX_Device &lcd, int d, int x, int y, int w, int h, int th,
                       uint32_t c0, uint32_t c1, int gy0, int gy1) {
  if (d < 0 || d > 9) return;
  for (int i = 0; i < CDIG_N[d]; i++)
    clockStroke(lcd, CDIG[d][i], x, y, w, h, th, c0, c1, gy0, gy1);
}

static void clockColon(LGFX_Device &lcd, int cx, int y, int h, int th,
                       uint32_t c0, uint32_t c1, int gy0, int gy1) {
  int r = th / 2;
  int y1 = y + h * 0.34f, y2 = y + h * 0.70f;
  lcd.fillCircle(cx, y1, r, clockGrad(lcd, c0, c1, y1, gy0, gy1));
  lcd.fillCircle(cx, y2, r, clockGrad(lcd, c0, c1, y2, gy0, gy1));
}

static const char *CWDAY[7] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};

// 主入口。自适应横竖屏: 横屏 HH:MM 排一行, 竖屏 HH 上 MM 下 —— 竖屏挤一行的话
// 每个数字只能有 55px 宽, 完全撑不起这种大字效果。
static void drawClockFace(LGFX_Device &lcd, const struct tm &tm_now,
                          uint32_t bg, uint32_t c0, uint32_t c1, uint32_t dim,
                          bool netOk) {
  int W = lcd.width(), H = lcd.height();
  lcd.fillScreen(lcd.color565((bg >> 16) & 0xFF, (bg >> 8) & 0xFF, bg & 0xFF));

  int hh = tm_now.tm_hour, mm = tm_now.tm_min;
  bool landscape = W > H;

  if (landscape) {
    int dateH = 0;                       // 横屏日期在右上角, 不占底部
    int dh = H * 0.58f, dw = dh * 0.60f, th = dh * 0.155f;
    int gap = dw * 0.16f, cw = th;
    int total = dw * 4 + gap * 4 + cw;
    int x = (W - total) / 2, y = (H - dh) / 2 + 6 + dateH;
    int gy0 = y, gy1 = y + dh;
    clockDigit(lcd, hh / 10, x, y, dw, dh, th, c0, c1, gy0, gy1); x += dw + gap;
    clockDigit(lcd, hh % 10, x, y, dw, dh, th, c0, c1, gy0, gy1); x += dw + gap;
    clockColon(lcd, x + cw / 2, y, dh, th, c0, c1, gy0, gy1);     x += cw + gap;
    clockDigit(lcd, mm / 10, x, y, dw, dh, th, c0, c1, gy0, gy1); x += dw + gap;
    clockDigit(lcd, mm % 10, x, y, dw, dh, th, c0, c1, gy0, gy1);
  } else {
    // 竖屏先扣掉日期要占的高度, 再决定数字多大 —— 之前是反过来先定数字尺寸,
    // 结果笔画的圆角半径超出标称box, 正好压在日期上
    const int dateH = 40;
    int avail = H - dateH;
    int dh = avail * 0.38f, dw = dh * 0.62f, th = dh * 0.155f;
    int gapY = avail * 0.05f, gapX = dw * 0.18f;
    int rowW = dw * 2 + gapX;
    int x0 = (W - rowW) / 2;
    int y1 = (avail - (dh * 2 + gapY)) / 2;
    int y2 = y1 + dh + gapY;
    int gy0 = y1, gy1 = y2 + dh;
    clockDigit(lcd, hh / 10, x0, y1, dw, dh, th, c0, c1, gy0, gy1);
    clockDigit(lcd, hh % 10, x0 + dw + gapX, y1, dw, dh, th, c0, c1, gy0, gy1);
    clockDigit(lcd, mm / 10, x0, y2, dw, dh, th, c0, c1, gy0, gy1);
    clockDigit(lcd, mm % 10, x0 + dw + gapX, y2, dw, dh, th, c0, c1, gy0, gy1);
  }

  // 日期。横屏放右上角(和参考图一致), 竖屏放底部居中
  char date[24];
  snprintf(date, sizeof(date), "%s %d", CWDAY[tm_now.tm_wday % 7], tm_now.tm_mday);
  lcd.setFont(&fonts::FreeSansBold12pt7b);
  lcd.setTextColor(TFT_WHITE);   // 日期用纯白, 和渐变大字拉开层次
  if (landscape) {
    lcd.setTextDatum(top_right);
    lcd.drawString(date, W - 14, 16);
  } else {
    lcd.setTextDatum(bottom_center);
    lcd.drawString(date, W / 2, H - 12);
  }

  // 时间源提示: 没有主机也没有网络时, 时间可能是不准的, 得让人看得出来
  if (!netOk) {
    lcd.setFont(&fonts::Font2);
    lcd.setTextColor(lcd.color565((dim >> 16) & 0xFF, (dim >> 8) & 0xFF, dim & 0xFF));
    lcd.setTextDatum(bottom_center);
    lcd.drawString("no time source", W / 2, H - 6);
  }
}
