#!/usr/bin/env python3
"""把 focus 扫描固件屏幕上显示的编号翻译回引脚定义。

用法: python3 decode_focus.py <编号>
必须和 src/focus/main.cpp 的 buildTable() 完全一致。
"""
import sys
from itertools import permutations

P5 = [2, 3, 4, 5, 7]


def build():
    tbl = []
    tri = [(4, 5, 7), (4, 7, 5), (5, 4, 7), (5, 7, 4), (7, 4, 5), (7, 5, 4)]
    for m in (0, 3):
        for dc, cs, rst in tri:
            tbl.append(dict(sck=2, mosi=3, dc=dc, cs=cs, rst=rst, mode=m))
    for m in (0, 3):
        for a, b, c, d, e in permutations(P5, 5):
            tbl.append(dict(sck=a, mosi=b, dc=c, cs=d, rst=e, mode=m))
    for m in (0, 3):
        for a, b, c in permutations(P5, 3):
            tbl.append(dict(sck=a, mosi=b, dc=c, cs=None, rst=None, mode=m))
    return tbl


PAD = {2: 19, 3: 20, 4: 28, 5: 27, 7: 23}  # GPIO -> 模块焊盘号


def show(i, t):
    def g(x):
        return "-" if x is None else f"GPIO{x}(焊盘{PAD[x]})"
    print(f"编号 {i}:")
    print(f"  SCK  = {g(t['sck'])}")
    print(f"  MOSI = {g(t['mosi'])}")
    print(f"  DC   = {g(t['dc'])}")
    print(f"  CS   = {g(t['cs'])}" + ("   (板上直接拉低)" if t["cs"] is None else ""))
    print(f"  RST  = {g(t['rst'])}" + ("   (未接 GPIO)" if t["rst"] is None else ""))
    print(f"  SPI mode = {t['mode']}")


if __name__ == "__main__":
    tbl = build()
    if len(sys.argv) < 2:
        print(f"共 {len(tbl)} 种组合。用法: decode_focus.py <编号>")
        sys.exit(0)
    i = int(sys.argv[1])
    if not 0 <= i < len(tbl):
        print(f"编号超范围, 应在 0..{len(tbl)-1}")
        sys.exit(1)
    show(i, tbl[i])
