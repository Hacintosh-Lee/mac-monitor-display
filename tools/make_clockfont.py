#!/usr/bin/env python3
"""把 EB Garamond Italic 的数字转成固件用的 8-bit 灰度字形表。

为什么是 8-bit 而不是常见的 1-bit 位图字体: Garamond 斜体有很细的发丝笔画,
1-bit 下会被切成断断续续的虚线。灰度蒙版保留抗锯齿, 上屏时按 alpha 混合,
顺便还能继续做渐变。

    python3 tools/make_clockfont.py          # 生成 include/clockfont.h

字体: EB Garamond (SIL OFL 1.1), 见 tools/fonts/OFL.txt
"""
import os
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
FONT = os.path.join(HERE, "fonts", "EBGaramond-Italic.ttf")
OUT = os.path.join(HERE, "..", "include", "clockfont.h")

WEIGHT = 600          # 可变字重: 400 纤细 / 600 扎实, 小屏远看 600 更清楚
SETS = [              # (名字, 像素尺寸, 需要的字符)
    ("BIG",   168, "0123456789"),      # 竖版: 两行两位, 不需要冒号
    ("SMALL", 126, "0123456789:"),     # 横版: HH:MM 一行
]


def render(ch, size):
    f = ImageFont.truetype(FONT, size)
    try:
        f.set_variation_by_axes([WEIGHT])
    except Exception:
        pass
    # 先量出这一档所有数字的公共基线和整体高度, 保证字形之间对齐
    pad = size // 4
    img = Image.new("L", (size * 2, size * 2), 0)
    ImageDraw.Draw(img).text((pad, pad), ch, font=f, fill=255)
    bb = img.getbbox()
    if bb is None:
        return None, 0, 0, 0, 0
    sub = img.crop(bb)
    adv = int(round(f.getlength(ch)))
    return sub, bb[0] - pad, bb[1] - pad, sub.width, adv


def emit(fh, name, size, chars):
    f = ImageFont.truetype(FONT, size)
    try:
        f.set_variation_by_axes([WEIGHT])
    except Exception:
        pass
    # 公共行高: 用所有字符的并集 bbox, 这样 dy 可以相对同一个顶边
    tops, bots = [], []
    for ch in chars:
        sub, dx, dy, w, adv = render(ch, size)
        if sub:
            tops.append(dy)
            bots.append(dy + sub.height)
    top0, boxh = min(tops), max(bots) - min(tops)

    entries, total = [], 0
    for ch in chars:
        sub, dx, dy, w, adv = render(ch, size)
        px = sub.tobytes()
        total += len(px)
        var = f"CLK{name}_{'C' if ch == ':' else ch}"
        fh.write(f"static const uint8_t {var}[] PROGMEM = {{")
        fh.write(",".join(str(b) for b in px))
        fh.write("};\n")
        entries.append((var, sub.width, sub.height, dx, dy - top0, adv))

    fh.write(f"\nstatic const ClkGlyph CLK{name}[] = {{\n")
    for var, w, h, dx, dy, adv in entries:
        fh.write(f"  {{{var}, {w}, {h}, {dx}, {dy}, {adv}}},\n")
    fh.write("};\n")
    # 斜体字形会超出标称步进宽度(advance): 向右倾斜的部分伸到下一格里去。
    # 导出全局墨迹范围, 上屏时按它算重绘区域, 否则右侧会被切掉。
    x0 = min(e[3] for e in entries)
    x1 = max(e[3] + e[1] for e in entries)
    fh.write(f"#define CLK{name}_COUNT {len(entries)}\n")
    fh.write(f"#define CLK{name}_H     {boxh}\n")
    fh.write(f"#define CLK{name}_X0    {x0}   // 墨迹最左(相对单元格原点)\n")
    fh.write(f"#define CLK{name}_X1    {x1}   // 墨迹最右, 可能 > adv\n\n")
    return total


with open(OUT, "w") as fh:
    fh.write("// 自动生成, 勿手改 —— 改字体/字号请跑 tools/make_clockfont.py\n")
    fh.write("// 字体: EB Garamond Italic (SIL OFL 1.1), 见 tools/fonts/OFL.txt\n")
    fh.write("#pragma once\n#include <Arduino.h>\n\n")
    fh.write("// 8-bit 灰度字形: a 指向 w*h 个 alpha 字节; dx/dy 是相对公共基准框的偏移,\n")
    fh.write("// adv 是步进宽度(含左右边距), 排版时用它而不是 w, 否则字距会忽宽忽窄。\n")
    fh.write("struct ClkGlyph { const uint8_t* a; uint16_t w, h; int16_t dx, dy; uint16_t adv; };\n\n")
    grand = 0
    for name, size, chars in SETS:
        n = emit(fh, name, size, chars)
        grand += n
        print(f"  {name}: 字号 {size}, {len(chars)} 个字形, {n//1024} KB")
    print(f"  合计 {grand//1024} KB")
print("已生成 include/clockfont.h")
