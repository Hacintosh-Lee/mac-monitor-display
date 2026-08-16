#!/usr/bin/env python3
"""生成三套界面的示意图。

这不是实拍照片, 是按 src/ 下各固件里的**实际版式常量和配色**重绘的渲染图 ——
改了版式重跑一次这个脚本就能同步更新, 不用重新拍照。

    python3 docs/make_mockups.py
"""
import os
from PIL import Image, ImageDraw, ImageFont

SCALE = 3
OUT = os.path.dirname(os.path.abspath(__file__))

HELV = "/System/Library/Fonts/Helvetica.ttc"
MENLO = "/System/Library/Fonts/Menlo.ttc"


def f(size, bold=True, mono=False):
    # Helvetica.ttc: index 1 = Bold;  Menlo.ttc: index 1 = Bold
    if mono:
        return ImageFont.truetype(MENLO, size * SCALE, index=1 if bold else 0)
    return ImageFont.truetype(HELV, size * SCALE, index=1 if bold else 0)


def S(v):
    return int(round(v * SCALE))


# 演示数据
D = dict(label="Mac mini M4", clock="14:32", cpu=42, gpu=57, ctemp=58, gtemp=56,
         mem_pct=73, mem="11.9 / 16.0 GB", ssd_pct=56, ssd="1.03 / 1.86 TB",
         rx="1.2 MB/s", tx="340 KB/s", rx_s="1.2M/s", tx_s="340K/s",
         load="3.07", up="4h 09m", top="WindowServer", topcpu="30%",
         cores=[46, 62, 31, 28, 18, 24, 35, 22, 14, 9],
         net=[4, 9, 17, 31, 52, 44, 28, 19, 33, 61, 78, 55, 34, 22, 16, 27, 41,
              36, 25, 18, 12, 21, 38, 49, 66, 47, 30, 23, 15, 11, 19, 29, 44,
              58, 71, 52, 37, 26, 20, 14, 10, 17, 26, 39, 51, 43, 31, 24, 18,
              13, 9, 15, 23, 34, 46, 38, 27, 20])


def rr(d, box, r, fill=None, outline=None, w=1):
    d.rounded_rectangle([S(box[0]), S(box[1]), S(box[2]), S(box[3])],
                        radius=S(r), fill=fill, outline=outline,
                        width=max(1, S(w) // 2))


def arc(d, cx, cy, r, th, a0, a1, col):
    """粗弧。PIL 的 arc 角度同样是 3 点钟起、顺时针增加, 和 LovyanGFX 一致。"""
    rr_ = r - th / 2
    box = [S(cx - rr_), S(cy - rr_), S(cx + rr_), S(cy + rr_)]
    d.arc(box, a0, a1, fill=col, width=S(th))


def heat(base, pct, warn="#FFD54F", hot="#FF5370"):
    if pct < 60:
        return base
    if pct < 85:
        return warn
    return hot


def txt(d, xy, s, font, col, anchor="lm"):
    d.text((S(xy[0]), S(xy[1])), s, font=font, fill=col, anchor=anchor)


def gauge(d, cx, cy, r, th, pct, base, label, temp, C):
    arc(d, cx, cy, r, th, 135, 405, C["track"])
    sweep = 270 * pct / 100
    # 深->亮渐变, 和固件里那 24 段的做法一致
    segs = 24
    for i in range(segs):
        a0 = 135 + sweep * i / segs
        a1 = 135 + sweep * (i + 1) / segs + 1
        t = i / segs
        c = tuple(int(int(base[1:][j:j + 2], 16) * (0.45 + 0.55 * t)) for j in (0, 2, 4))
        arc(d, cx, cy, r, th, a0, min(a1, 135 + sweep), c if pct < 60 else heat(base, pct))
    txt(d, (cx, cy - 8), str(pct), f(19), C["text"], "mm")
    txt(d, (cx + 22, cy - 1), "%", f(8, False), C["dim"], "lm")
    txt(d, (cx, cy + 16), f"{temp}°C", f(10), C["temp"], "mm")
    txt(d, (cx, cy + r + 12), label, f(10), heat(base, pct), "mm")


# ============================================================
def dark_portrait():
    C = dict(bg="#0A0D14", card="#121A26", track="#1E2836", text="#E6EDF3",
             dim="#6B7A8F", cpu="#00E5FF", gpu="#B388FF", mem="#FFB74D",
             disk="#4DD0A1", rx="#4FC3F7", tx="#FF8A80", temp="#B8C4D4")
    im = Image.new("RGB", (S(240), S(320)), C["bg"])
    d = ImageDraw.Draw(im)

    # 顶栏
    txt(d, (8, 15), D["label"], f(10), C["text"])
    txt(d, (232, 15), D["clock"], f(13), C["cpu"], "rm")
    for x in range(240):
        t = x / 239
        c = tuple(int(int(C["cpu"][1:][j:j+2], 16) * (1 - t) + int(C["gpu"][1:][j:j+2], 16) * t) for j in (0, 2, 4))
        d.rectangle([S(x), S(29), S(x + 1), S(29) + SCALE - 1], fill=c)

    gauge(d, 61, 32 + 50, 42, 12, D["cpu"], C["cpu"], "CPU", D["ctemp"], C)
    gauge(d, 179, 32 + 50, 42, 12, D["gpu"], C["gpu"], "GPU", D["gtemp"], C)

    # 核心条
    n, gap, w = 10, 3, 20
    x = (240 - (w * n + gap * (n - 1))) // 2
    for v in D["cores"]:
        rr(d, (x, 151, x + w, 166), 2, C["track"])
        h = int(15 * v / 100)
        rr(d, (x, 166 - h, x + w, 166), 2, heat(C["cpu"], v))
        x += w + gap

    # 内存 / 存储
    for y, lab, pct, sub, base in ((172, "MEM", D["mem_pct"], D["mem"], C["mem"]),
                                   (206, "SSD", D["ssd_pct"], D["ssd"], C["disk"])):
        txt(d, (28, y + 6), lab, f(10, mono=True), C["dim"], "mm")
        rr(d, (52, y, 192, y + 12), 6, C["track"])
        rr(d, (52, y, 52 + int(140 * pct / 100), y + 12), 6, heat(base, pct))
        txt(d, (232, y + 6), f"{pct}%", f(10), C["text"], "rm")
        txt(d, (52, y + 22), sub, f(8, False), C["dim"])

    # 网络
    d.polygon([(S(12), S(256)), (S(8), S(249)), (S(16), S(249))], fill=C["rx"])
    txt(d, (22, 251), D["rx"], f(10), C["text"])
    txt(d, (232, 251), D["tx"], f(10), C["text"], "rm")
    d.polygon([(S(168), S(245)), (S(164), S(252)), (S(172), S(252))], fill=C["tx"])
    rr(d, (4, 263, 236, 289), 3, C["card"])
    for i, v in enumerate(D["net"]):
        bh = int(v * 22 / 100)
        if bh:
            d.rectangle([S(6 + i * 4), S(287 - bh), S(6 + i * 4 + 3), S(287)], fill=C["rx"])

    # 底栏
    txt(d, (8, 299), "LOAD", f(8, False), C["dim"])
    txt(d, (44, 299), D["load"], f(8, False), C["cpu"])
    txt(d, (94, 299), "UP", f(8, False), C["dim"])
    txt(d, (116, 299), D["up"], f(8, False), C["text"])
    txt(d, (8, 315), D["top"], f(8, False), C["gpu"])
    txt(d, (232, 315), D["topcpu"], f(8, False), C["dim"], "rm")
    im.save(os.path.join(OUT, "ui-dark-portrait.png"))


# ============================================================
def dark_landscape():
    C = dict(bg="#0A0D14", card="#121A26", track="#1E2836", text="#E6EDF3",
             dim="#6B7A8F", cpu="#00E5FF", gpu="#B388FF", mem="#FFB74D",
             disk="#4DD0A1", rx="#4FC3F7", tx="#FF8A80", temp="#B8C4D4")
    im = Image.new("RGB", (S(320), S(240)), C["bg"])
    d = ImageDraw.Draw(im)

    txt(d, (10, 15), D["label"], f(13), C["text"])
    txt(d, (312, 15), D["clock"], f(13), C["cpu"], "rm")
    for x in range(320):
        t = x / 319
        c = tuple(int(int(C["cpu"][1:][j:j+2], 16) * (1 - t) + int(C["gpu"][1:][j:j+2], 16) * t) for j in (0, 2, 4))
        d.rectangle([S(x), S(29), S(x + 1), S(29) + SCALE - 1], fill=c)

    gauge(d, 42, 34 + 52, 40, 11, D["cpu"], C["cpu"], "CPU", D["ctemp"], C)
    gauge(d, 126, 34 + 52, 40, 11, D["gpu"], C["gpu"], "GPU", D["gtemp"], C)

    # 右列 内存/存储
    for y, lab, pct, sub, base in ((42, "MEM", D["mem_pct"], D["mem"], C["mem"]),
                                   (82, "SSD", D["ssd_pct"], D["ssd"], C["disk"])):
        txt(d, (172 + 24, y + 6), lab, f(10, mono=True), C["dim"], "mm")
        rr(d, (172 + 48, y, 172 + 108, y + 12), 6, C["track"])
        rr(d, (172 + 48, y, 172 + 48 + int(60 * pct / 100), y + 12), 6, heat(base, pct))
        txt(d, (172 + 142, y + 6), f"{pct}%", f(8, False), C["text"], "rm")
        txt(d, (172 + 48, y + 24), sub, f(8, False), C["dim"])

    # 右列 网络
    d.polygon([(S(182), S(132)), (S(178), S(125)), (S(186), S(125))], fill=C["rx"])
    txt(d, (190, 127), D["rx_s"], f(8, False), C["text"])
    txt(d, (314, 127), D["tx_s"], f(8, False), C["text"], "rm")
    d.polygon([(S(268), S(121)), (S(264), S(128)), (S(272), S(128))], fill=C["tx"])
    rr(d, (174, 138, 318, 162), 3, C["card"])
    for i, v in enumerate(D["net"]):
        bh = int(v * 20 / 100)
        if bh:
            d.rectangle([S(176 + i * 2), S(160 - bh), S(176 + i * 2 + 1), S(160)], fill=C["rx"])

    # 核心条横贯全宽
    n, gap = 10, 3
    w = (304 - gap * (n - 1)) // n
    x = (320 - (w * n + gap * (n - 1))) // 2
    for v in D["cores"]:
        rr(d, (x, 170, x + w, 192), 2, C["track"])
        h = int(22 * v / 100)
        rr(d, (x, 192 - h, x + w, 192), 2, heat(C["cpu"], v))
        x += w + gap

    txt(d, (10, 215), "LOAD", f(8, False), C["dim"])
    txt(d, (48, 215), D["load"], f(8, False), C["cpu"])
    txt(d, (100, 215), "UP", f(8, False), C["dim"])
    txt(d, (124, 215), D["up"], f(8, False), C["text"])
    txt(d, (310, 215), D["topcpu"], f(8, False), C["dim"], "rm")
    txt(d, (280, 215), D["top"], f(8, False), C["gpu"], "rm")
    im.save(os.path.join(OUT, "ui-dark-landscape.png"))


# ============================================================
def light_portrait():
    C = dict(bg="#F2F2F7", card="#FFFFFF", hair="#E5E5EA", track="#E9E9EB",
             label="#1C1C1E", label2="#8E8E93", blue="#007AFF", purple="#AF52DE",
             orange="#FF9500", green="#34C759", cyan="#32ADE6", pink="#FF2D55")
    im = Image.new("RGB", (S(240), S(320)), C["bg"])
    d = ImageDraw.Draw(im)

    d.ellipse([S(12), S(14), S(20), S(22)], fill=C["green"])
    txt(d, (26, 18), D["label"], f(10), C["label"])
    txt(d, (230, 18), D["clock"], f(13), C["label"], "rm")

    # 双环卡片
    for x0, pct, base, lab in ((10, D["cpu"], C["blue"], "CPU"),
                               (125, D["gpu"], C["purple"], "GPU")):
        rr(d, (x0, 38, x0 + 105, 146), 14, C["card"], C["hair"])
        cx, cy = x0 + 52, 84
        arc(d, cx, cy, 34, 9, 135, 405, C["track"])
        arc(d, cx, cy, 34, 9, 135, 135 + 270 * pct / 100, base)
        txt(d, (cx, cy - 2), str(pct), f(13), C["label"], "mm")
        txt(d, (cx, cy + 16), "%", f(8, False), C["label2"], "mm")
        txt(d, (cx, cy + 44), lab, f(10), base, "mm")

    # 核心
    rr(d, (10, 150, 230, 184), 14, C["card"], C["hair"])
    n, gap = 10, 3
    w = (196 - gap * (n - 1)) // n
    x = 22 + (196 - (w * n + gap * (n - 1))) // 2
    for v in D["cores"]:
        rr(d, (x, 158, x + w, 176), 2, C["track"])
        h = int(18 * v / 100)
        rr(d, (x, 176 - h, x + w, 176), 2, C["blue"])
        x += w + gap

    # 内存 / 存储
    rr(d, (10, 188, 230, 248), 14, C["card"], C["hair"])
    for y, lab, pct, sub, base in ((202, "Memory", D["mem_pct"], D["mem"], C["orange"]),
                                   (228, "Storage", D["ssd_pct"], D["ssd"], C["green"])):
        txt(d, (24, y), lab, f(10), C["label"])
        txt(d, (216, y), sub, f(8, False), C["label2"], "rm")
        rr(d, (24, y + 12, 216, y + 18), 3, C["track"])
        rr(d, (24, y + 12, 24 + int(192 * pct / 100), y + 18), 3, base)

    # 网络
    rr(d, (10, 252, 230, 298), 14, C["card"], C["hair"])
    d.polygon([(S(28), S(265)), (S(24), S(258)), (S(32), S(258))], fill=C["cyan"])
    txt(d, (38, 260), D["rx"], f(10), C["label"])
    txt(d, (216, 260), D["tx"], f(10), C["label"], "rm")
    d.polygon([(S(160), S(254)), (S(156), S(261)), (S(164), S(261))], fill=C["pink"])
    rr(d, (20, 272, 220, 292), 3, "#F7F7FA")
    for i, v in enumerate(D["net"]):
        bh = int(v * 16 / 100)
        if bh:
            d.rectangle([S(22 + i * 3), S(290 - bh), S(22 + i * 3 + 2), S(290)], fill=C["cyan"])

    txt(d, (14, 308), f"Load {D['load']}   Up {D['up']}", f(8, False), C["label2"])
    txt(d, (226, 308), f"{D['top']} {D['topcpu']}", f(8, False), C["purple"], "rm")
    im.save(os.path.join(OUT, "ui-light-portrait.png"))


# ============================================================
# 待机时钟。路径定义和 include/clockface.h 里的 CDIG 一一对应,
# 改了字形两边都要动 —— 但至少 README 的图不会和固件长得不一样。
CDIG = {
    0: [("a", .50, .50, .32, .42, 0, 360)],
    1: [("l", .54, .07, .54, .93), ("l", .54, .09, .25, .28)],
    2: [("a", .50, .30, .30, .22, 180, 375), ("l", .79, .36, .14, .90),
        ("l", .14, .90, .88, .90)],
    3: [("a", .48, .30, .28, .22, 180, 450), ("a", .48, .74, .32, .22, 270, 540)],
    4: [("l", .68, .08, .09, .63), ("l", .09, .63, .92, .63), ("l", .68, .08, .68, .92)],
    5: [("l", .82, .09, .22, .09), ("l", .22, .09, .20, .50),
        ("a", .48, .66, .30, .27, 215, 505)],
    6: [("a", .52, .31, .27, .23, 180, 345), ("l", .25, .31, .19, .68),
        ("a", .50, .70, .31, .23, 0, 360)],
    7: [("l", .11, .09, .89, .09), ("l", .89, .09, .36, .93)],
    8: [("a", .50, .30, .27, .22, 0, 360), ("a", .50, .72, .31, .22, 0, 360)],
    9: [("a", .48, .30, .29, .23, 0, 360), ("l", .77, .30, .79, .66),
        ("a", .48, .66, .31, .24, 0, 125)],
}


def _grad(c0, c1, y, y0, y1):
    t = 0 if y1 <= y0 else max(0.0, min(1.0, (y - y0) / (y1 - y0)))
    return tuple(int(int(c0[1:][j:j+2], 16) * (1 - t) + int(c1[1:][j:j+2], 16) * t)
                 for j in (0, 2, 4))


def _stamp(d, px, py, r, col):
    d.ellipse([S(px - r), S(py - r), S(px + r), S(py + r)], fill=col)


def _digit(d, n, x, y, w, h, th, c0, c1, gy0, gy1):
    import math
    r = th / 2
    for seg in CDIG[n]:
        if seg[0] == "l":
            _, x1, y1, x2, y2 = seg
            X1, Y1, X2, Y2 = x + x1 * w, y + y1 * h, x + x2 * w, y + y2 * h
            steps = int(math.hypot(X2 - X1, Y2 - Y1) / 0.7) + 1
            for i in range(steps + 1):
                t = i / steps
                px, py = X1 + (X2 - X1) * t, Y1 + (Y2 - Y1) * t
                _stamp(d, px, py, r, _grad(c0, c1, py, gy0, gy1))
        else:
            _, cx, cy, rx, ry, a0, a1 = seg
            CX, CY, RX, RY = x + cx * w, y + cy * h, rx * w, ry * h
            steps = int(abs(a1 - a0) * math.pi / 180 * (RX + RY) / 2 / 0.7) + 1
            for i in range(steps + 1):
                a = math.radians(a0 + (a1 - a0) * i / steps)
                px, py = CX + math.cos(a) * RX, CY + math.sin(a) * RY
                _stamp(d, px, py, r, _grad(c0, c1, py, gy0, gy1))


def clock_portrait():
    BG, C0, C1 = "#0A0D14", "#00E5FF", "#B388FF"
    W, H = 240, 320
    im = Image.new("RGB", (S(W), S(H)), BG)
    d = ImageDraw.Draw(im)

    dateH = 40
    avail = H - dateH
    dh = avail * 0.38; dw = dh * 0.62; th = dh * 0.155
    gapY = avail * 0.05; gapX = dw * 0.18
    x0 = (W - (dw * 2 + gapX)) / 2
    y1 = (avail - (dh * 2 + gapY)) / 2
    y2 = y1 + dh + gapY
    gy0, gy1 = y1, y2 + dh

    hh, mm = 9, 41
    _digit(d, hh // 10, x0, y1, dw, dh, th, C0, C1, gy0, gy1)
    _digit(d, hh % 10, x0 + dw + gapX, y1, dw, dh, th, C0, C1, gy0, gy1)
    _digit(d, mm // 10, x0, y2, dw, dh, th, C0, C1, gy0, gy1)
    _digit(d, mm % 10, x0 + dw + gapX, y2, dw, dh, th, C0, C1, gy0, gy1)

    txt(d, (W / 2, H - 20), "SUN 16", f(12), "#FFFFFF", "mm")
    im.save(os.path.join(OUT, "ui-clock-portrait.png"))
    print("  ui-clock-portrait.png")


if __name__ == "__main__":
    dark_portrait()
    dark_landscape()
    light_portrait()
    clock_portrait()
    print("已生成:")
    for n in ("ui-dark-portrait.png", "ui-dark-landscape.png",
              "ui-light-portrait.png", "ui-clock-portrait.png"):
        fp = os.path.join(OUT, n)
        print(f"  {n}  {os.path.getsize(fp) // 1024} KB")
