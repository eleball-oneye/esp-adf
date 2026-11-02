# -*- coding: utf-8 -*-
"""在 PC 上渲染「板载屏应该显示成什么样」——用于和真机对照（排障工具，不参与固件构建）。

它**逐条复刻** `main/board_lcd.c` 的绘制逻辑（卡片/暗化、8x16 点阵字、WiFi 扇形、温度大数字），
字形直接从 `main/lcd_font.h` 解析 ⇒ 预览与真机像素级同源。

版面（2026-10-25 用户要求的「顶部 logo + 下方分栏卡片」）：
    y   0..44   底图：顶部奶油 logo
    y  46..146  卡片 A（大）：温度（整数 4 倍字 + 小数 2 倍字）
    y 152..238  卡片 B（左，略小）：WiFi 图标/dBm/等级 ｜ 卡片 C（右，略小）：机型/电量/云+子设备灯

用法：
    python tools/preview-lcd.py <背景 png> <输出 png> [--temp 36.5] [--rssi -45]
                              [--no-wifi] [--sub-on] [--batt 88] [--scale 3]
"""
import argparse
import re
import sys

from PIL import Image

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

W, H = 135, 240
CREAM = 0xF6F8
WHITE = 0xFFFF
RED = 0xF800
GREY = 0x7BEF
GL = {}
GLBIG = {}


def rgb565(v):
    r = ((v >> 11) & 0x1F) * 255 // 31
    g = ((v >> 5) & 0x3F) * 255 // 63
    b = (v & 0x1F) * 255 // 31
    return (r, g, b)


def load_glyphs(header_path):
    txt = open(header_path, encoding="utf-8").read()
    out = {}
    for m in re.finditer(r"\{\s*'(.)'\s*,\s*\{([^}]*)\}\s*\}", txt):
        ch = m.group(1)
        rows = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(2))]
        if len(rows) == 16:
            out[ch] = rows
    return out


def load_glyphs_big(header_path):
    """16×24 大字（温度用）：每行 2 字节、共 24 行 ⇒ 48 字节。"""
    txt = open(header_path, encoding="utf-8").read()
    out = {}
    for m in re.finditer(r"\{\s*'(.)'\s*,\s*\{([^}]*)\}\s*\}", txt):
        ch = m.group(1)
        rows = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(2))]
        if len(rows) == 48:
            out[ch] = rows
    return out


class Canvas:
    def __init__(self, bg_path):
        self.im = Image.open(bg_path).convert("RGB")
        if self.im.size != (W, H):
            self.im = self.im.resize((W, H), Image.LANCZOS)
        self.px = self.im.load()

    def fill(self, x, y, w, h, v):
        c = rgb565(v)
        for yy in range(max(0, y), min(H, y + h)):
            for xx in range(max(0, x), min(W, x + w)):
                self.px[xx, yy] = c

    def tx(self, x, y, ch, scale, v):
        rows = GL.get(ch)
        if rows is None:
            return
        c = rgb565(v)
        for gy in range(16):
            bits = rows[gy]
            for gx in range(8):
                if bits & (0x80 >> gx):
                    for dy in range(scale):
                        for dx in range(scale):
                            xx, yy = x + gx * scale + dx, y + gy * scale + dy
                            if 0 <= xx < W and 0 <= yy < H:
                                self.px[xx, yy] = c

    def text(self, x, y, s, scale, v):
        for ch in s:
            self.tx(x, y, ch, scale, v)
            x += 9 * scale
        return x

    def tx_big(self, x, y, ch, scale, v):
        rows = GLBIG.get(ch)
        if rows is None:
            return
        c = rgb565(v)
        for gy in range(24):
            for gx in range(16):
                if rows[gy * 2 + gx // 8] & (0x80 >> (gx % 8)):
                    for dy in range(scale):
                        for dx in range(scale):
                            xx, yy = x + gx * scale + dx, y + gy * scale + dy
                            if 0 <= xx < W and 0 <= yy < H:
                                self.px[xx, yy] = c

    def text_big(self, x, y, s, scale, v):
        for ch in s:
            self.tx_big(x, y, ch, scale, v)
            x += 18 * scale
        return x

    def panel(self, x, y, w, h):
        """卡片：内部压到 1/4 亮度 + 1 px 奶油描边（与 board_lcd.c 的 fb_panel 一致）。"""
        for yy in range(y + 1, y + h - 1):
            for xx in range(x + 1, x + w - 1):
                r, g, b = self.px[xx, yy]
                self.px[xx, yy] = (r // 4, g // 4, b // 4)
        self.fill(x, y, w, 1, CREAM)
        self.fill(x, y + h - 1, w, 1, CREAM)
        self.fill(x, y, 1, h, CREAM)
        self.fill(x + w - 1, y, 1, h, CREAM)


def text_width(s, scale):
    return len(s) * 9 * scale


def wifi(c, cx, base, connected, level, on_col):
    for ring, r in enumerate((4, 9, 14)):
        lit = connected and (level >= (3 - ring))
        if connected and level == 0:
            lit = (ring == 2)
        col = on_col if lit else GREY
        r_out, r_in = (r + 1) ** 2, (r - 1) ** 2
        for dy in range(-(r + 2), 1):
            for dx in range(-(r + 2), r + 3):
                d2 = dx * dx + dy * dy
                if not (r_in <= d2 <= r_out):
                    continue
                if abs(dx) * 100 > r * 85:
                    continue
                xx, yy = cx + dx, base + dy
                if 0 <= xx < W and 0 <= yy < H:
                    c.px[xx, yy] = rgb565(col)
    c.fill(cx - 1, base - 1, 3, 3, on_col if connected else GREY)
    if not connected:
        for i in range(26):
            c.fill(cx - 13 + i, base - 14 + i // 2, 2, 2, RED)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("bg")
    ap.add_argument("out")
    ap.add_argument("--font", default="main/lcd_font.h")
    ap.add_argument("--font-big", default="main/lcd_font_big.h")
    ap.add_argument("--temp", default=None)
    ap.add_argument("--rssi", type=int, default=-45)
    ap.add_argument("--no-wifi", action="store_true")
    ap.add_argument("--sub-on", action="store_true")
    ap.add_argument("--batt", type=int, default=-1)
    ap.add_argument("--scale", type=int, default=3)
    a = ap.parse_args()

    global GL, GLBIG
    GL = load_glyphs(a.font)
    GLBIG = load_glyphs_big(a.font_big)
    c = Canvas(a.bg)
    conn = not a.no_wifi
    level = 3 if a.rssi >= -55 else 2 if a.rssi >= -67 else 1 if a.rssi >= -78 else 0

    # ---- 卡片 A：温度 ----
    c.panel(4, 46, W - 8, 100)
    c.text(10, 50, "TEMP", 1, CREAM)
    if a.temp is None:
        c.text(10, 66, "WAIT SUB", 1, GREY)
        whole, frac = "--", ".-"
    else:
        if not a.sub_on:
            c.text(10, 66, "SUB OFFLINE", 1, GREY)
        s = f"{a.temp}"
        whole = s.split(".")[0]
        frac = "." + (s.split(".")[1] if "." in s else "0")
    wq, wf = len(whole) * 17 * 2, len(frac) * 17
    deg_w = 14
    x0 = (W - (wq + wf + deg_w)) // 2
    y_t = 84
    c.text_big(x0, y_t, whole, 2, WHITE)
    c.text_big(x0 + wq, y_t + 24, frac, 1, WHITE)
    xd = x0 + wq + wf + 4
    c.fill(xd, y_t + 4, 4, 4, CREAM)
    c.fill(xd + 1, y_t + 5, 2, 2, 0)
    c.text(xd + 5, y_t + 8, "C", 1, CREAM)

    # ---- 卡片 B（左，略小）：WiFi ----
    c.panel(4, 152, 62, 86)
    c.text(8, 156, "WiFi", 1, CREAM)
    wifi(c, 35, 196, conn, level if conn else 0, CREAM)
    txt = f"{a.rssi}dBm" if conn else "OFF"
    c.text(4 + (62 - text_width(txt, 1)) // 2, 200, txt, 1, CREAM if conn else RED)
    lv = ("GOOD" if level >= 3 else "FAIR" if level == 2 else "WEAK" if level == 1 else "POOR") if conn else "NO NET"
    c.text(4 + (62 - text_width(lv, 1)) // 2, 218, lv, 1, CREAM if conn else GREY)

    # ---- 卡片 C（右，略小）：机型 / 电量 / 云+子设备灯 ----
    c.panel(70, 152, W - 74, 86)
    c.text(74, 156, "T31", 1, CREAM)
    bat = "--%" if a.batt < 0 else f"{a.batt}%"
    c.text(74, 176, bat, 2, WHITE)
    c.fill(74, 216, 8, 8, CREAM)                      # 云灯（预览默认在线）
    c.text(84, 212, "C", 1, GREY)
    c.fill(102, 216, 8, 8, CREAM if a.sub_on else GREY)  # 子设备灯
    c.text(112, 212, "S", 1, GREY)

    s = a.scale
    c.im.resize((W * s, H * s), Image.NEAREST).save(a.out)
    print(f"已渲染预览 {a.out}（{W}x{H}，放大 {s} 倍便于查看）")


if __name__ == "__main__":
    sys.exit(main())
