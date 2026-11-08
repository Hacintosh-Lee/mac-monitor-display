// 大字时钟, 参考 iOS StandBy: 巨大渐变数字 + 角落日期。
//
// 字形来自 EB Garamond Italic (SIL OFL), 由 tools/make_clockfont.py 转成
// 8-bit 灰度蒙版编进固件。用灰度而不是 1-bit 位图字体, 是因为 Garamond 斜体
// 有很细的发丝笔画, 1-bit 下会被切成断断续续的虚线。
//
// 刷新策略: 只重画发生变化的那一位数字。数字是等宽的(adv 相同), 所以每位占一个
// 固定单元格; 换分钟时通常只有一两格变, 整屏重刷会看到明显闪烁, 也白白推了
// 十几万像素。进入时钟模式的第一帧才做整屏清屏。
//
// 合成方式是逐行拼好再推该行的那一段: 只要一个行缓冲(480 字节), 不用整屏精灵
// (那要 150KB, 和仪表盘的精灵抢不过来), 而且一行一次成像看不到中间态。
#pragma once
#include <Arduino.h>
#include "clockfont.h"

#define CLK_SLOTS 5                 // 最多 5 位 (HH:MM)

static uint16_t clkRow[340];

static inline uint16_t clkRGB(LGFX_Device &lcd, uint32_t c) {
    return lcd.color565((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
}

static inline uint16_t clkBlend(LGFX_Device &lcd, uint32_t bg, uint32_t fg, uint8_t a) {
    if (a == 0)   return clkRGB(lcd, bg);
    if (a == 255) return clkRGB(lcd, fg);
    int r = (((fg >> 16) & 0xFF) * a + ((bg >> 16) & 0xFF) * (255 - a)) / 255;
    int g = (((fg >> 8)  & 0xFF) * a + ((bg >> 8)  & 0xFF) * (255 - a)) / 255;
    int b = ((fg & 0xFF) * a + (bg & 0xFF) * (255 - a)) / 255;
    return lcd.color565(r, g, b);
}

static inline uint32_t clkGradRGB(uint32_t c0, uint32_t c1, int y, int y0, int y1) {
    int span = y1 - y0;
    int t = (span > 0) ? ((y - y0) * 255 / span) : 0;
    if (t < 0) t = 0;
    if (t > 255) t = 255;
    int r = (((c0 >> 16) & 0xFF) * (255 - t) + ((c1 >> 16) & 0xFF) * t) / 255;
    int g = (((c0 >> 8)  & 0xFF) * (255 - t) + ((c1 >> 8)  & 0xFF) * t) / 255;
    int b = ((c0 & 0xFF) * (255 - t) + (c1 & 0xFF) * t) / 255;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

// 重画一个横向区间: 铺底色, 再把所有与之相交的字形按 alpha 混上去。
//
// 为什么要一次合成多个字形: 斜体的墨迹会超出标称步进宽度(adv), 向右倾斜的部分
// 伸进下一格。所以重绘区域必须按墨迹算(而不是 adv), 而这个区域又可能盖住邻居
// 伸过来的笔画 —— 只画自己就会把邻居擦掉一块。
static void clkPaintSpan(LGFX_Device &lcd, int rx, int rw, int ry, int rh,
                         const ClkGlyph* const* gs, const int* gx, const int* gy,
                         int n, int gy0, int gy1,
                         uint32_t bg, uint32_t c0, uint32_t c1) {
    int W = lcd.width(), H = lcd.height();
    if (rx < 0) { rw += rx; rx = 0; }
    if (rx + rw > W) rw = W - rx;
    if (rw <= 0) return;

    uint16_t bgc = clkRGB(lcd, bg);
    for (int y = ry; y < ry + rh; y++) {
        if (y < 0 || y >= H) continue;
        for (int i = 0; i < rw; i++) clkRow[i] = bgc;

        uint32_t fg = clkGradRGB(c0, c1, y, gy0, gy1);
        for (int k = 0; k < n; k++) {
            const ClkGlyph* g = gs[k];
            int row = y - gy[k];
            if (row < 0 || row >= (int)g->h) continue;
            const uint8_t* src = g->a + (size_t)row * g->w;
            for (int i = 0; i < (int)g->w; i++) {
                int x = gx[k] + i - rx;
                if (x < 0 || x >= rw) continue;
                uint8_t aa = src[i];
                if (aa) clkRow[x] = clkBlend(lcd, bg, fg, aa);
            }
        }
        // 必须显式指定颜色类型: pushImage 收到裸 uint16_t* 时按字节交换序解释,
        // 而 color565() 返回主机序。不指定的话颜色和底色都会错。
        lcd.pushImage(rx, y, rw, 1, (const lgfx::rgb565_t*)clkRow);
    }
}

static const char *CWDAY[7] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};

// full=true 时整屏重画(刚从仪表盘切过来); 否则只重画变化的那几位
static void drawClockFace(LGFX_Device &lcd, const struct tm &tm_now,
                          uint32_t bg, uint32_t c0, uint32_t c1, uint32_t dim,
                          bool netOk, bool full) {
    int W = lcd.width(), H = lcd.height();
    bool landscape = W > H;

    // ---- 排版 ----
    const ClkGlyph* seq[CLK_SLOTS];
    int sx[CLK_SLOTS], sy[CLK_SLOTS], sw[CLK_SLOTS];
    int n = 0, cellH, gy0, gy1, dateY;
    int hh = tm_now.tm_hour, mm = tm_now.tm_min;
    char code[CLK_SLOTS];

    if (landscape) {
        const ClkGlyph* q[5] = {&CLKSMALL[hh / 10], &CLKSMALL[hh % 10], &CLKSMALL[10],
                                &CLKSMALL[mm / 10], &CLKSMALL[mm % 10]};
        const char cc[5] = {(char)('0' + hh / 10), (char)('0' + hh % 10), ':',
                            (char)('0' + mm / 10), (char)('0' + mm % 10)};
        int total = 0;
        for (int i = 0; i < 5; i++) total += q[i]->adv;
        int x = (W - total) / 2, y = (H - CLKSMALL_H) / 2 + 6;
        cellH = CLKSMALL_H; gy0 = y; gy1 = y + CLKSMALL_H;
        for (int i = 0; i < 5; i++) {
            seq[n] = q[i]; sx[n] = x; sy[n] = y; sw[n] = q[i]->adv; code[n] = cc[i];
            x += q[i]->adv; n++;
        }
        dateY = 16;
    } else {
        const int dateH = 34;
        int gap = CLKBIG_H / 14;
        int blockH = CLKBIG_H * 2 + gap;
        int y = (H - dateH - blockH) / 2;
        cellH = CLKBIG_H; gy0 = y; gy1 = y + blockH;
        int d[4] = {hh / 10, hh % 10, mm / 10, mm % 10};
        for (int r = 0; r < 2; r++) {
            const ClkGlyph* a = &CLKBIG[d[r * 2]];
            const ClkGlyph* b = &CLKBIG[d[r * 2 + 1]];
            int x = (W - (a->adv + b->adv)) / 2;
            seq[n] = a; sx[n] = x; sy[n] = y; sw[n] = a->adv; code[n] = (char)('0' + d[r*2]); n++;
            x += a->adv;
            seq[n] = b; sx[n] = x; sy[n] = y; sw[n] = b->adv; code[n] = (char)('0' + d[r*2+1]); n++;
            y += CLKBIG_H + gap;
        }
        dateY = H - 12;
    }

    // ---- 只重画变化的位 ----
    static char lastCode[CLK_SLOTS] = {0, 0, 0, 0, 0};
    static int lastMday = -1;

    if (full) {
        lcd.fillScreen(clkRGB(lcd, bg));
        for (int i = 0; i < CLK_SLOTS; i++) lastCode[i] = 0;
        lastMday = -1;
    }

    int inkX0 = landscape ? CLKSMALL_X0 : CLKBIG_X0;
    int inkX1 = landscape ? CLKSMALL_X1 : CLKBIG_X1;

    // 按行处理: 竖版两行各两位, 横版一行五位
    int rowCount = landscape ? 1 : 2;
    int perRow   = landscape ? 5 : 2;
    for (int r = 0; r < rowCount; r++) {
        int lo = r * perRow, hi = lo + perRow;
        int dx0 = 32767, dx1 = -32768;
        for (int i = lo; i < hi && i < n; i++) {
            if (code[i] == lastCode[i]) continue;
            if (sx[i] + inkX0 < dx0) dx0 = sx[i] + inkX0;
            if (sx[i] + inkX1 > dx1) dx1 = sx[i] + inkX1;
        }
        if (dx1 <= dx0) continue;            // 这一行没有变化

        // 把本行所有与脏区相交的字形都收进来一起合成
        const ClkGlyph* gs[CLK_SLOTS];
        int gx[CLK_SLOTS], gy[CLK_SLOTS], m = 0;
        for (int i = lo; i < hi && i < n; i++) {
            int ix0 = sx[i] + seq[i]->dx, ix1 = ix0 + seq[i]->w;
            if (ix1 <= dx0 || ix0 >= dx1) continue;
            gs[m] = seq[i]; gx[m] = ix0; gy[m] = sy[i] + seq[i]->dy; m++;
        }
        clkPaintSpan(lcd, dx0, dx1 - dx0, sy[lo], cellH, gs, gx, gy, m,
                     gy0, gy1, bg, c0, c1);
        for (int i = lo; i < hi && i < n; i++) lastCode[i] = code[i];
    }

    // ---- 日期每天才变一次 ----
    if (full || tm_now.tm_mday != lastMday) {
        lastMday = tm_now.tm_mday;
        char date[24];
        snprintf(date, sizeof(date), "%s %d", CWDAY[tm_now.tm_wday % 7], tm_now.tm_mday);
        lcd.setFont(&fonts::FreeSansBold12pt7b);
        lcd.setTextColor(TFT_WHITE, clkRGB(lcd, bg));   // 带背景色, 自己擦掉上一次
        if (landscape) {
            lcd.setTextDatum(top_right);
            lcd.drawString(date, W - 14, dateY);
        } else {
            lcd.setTextDatum(bottom_center);
            lcd.drawString(date, W / 2, dateY);
        }
        if (!netOk) {
            lcd.setFont(&fonts::Font2);
            lcd.setTextColor(clkRGB(lcd, dim), clkRGB(lcd, bg));
            lcd.setTextDatum(bottom_center);
            lcd.drawString(" no time source ", W / 2, H - 2);
        }
    }
}
