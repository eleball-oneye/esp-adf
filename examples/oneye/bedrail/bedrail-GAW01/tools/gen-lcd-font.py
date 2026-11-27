# -*- coding: utf-8 -*-
"""生成 LCD 用的两套位图字体（都从系统等宽粗体二值化而来）：

  ① `lcd_font.h`      —— **8×16**，全可打印 ASCII（95 字形）。小字栏（标题/WiFi/机型/电量）用它。
  ② `lcd_font_big.h`  —— **16×24**，只有温度用的字形（0-9 . -）。温度大字用它（再放大 2 倍 ⇒ 32×48），
     比"8×16 放大 4 倍"清楚得多（4 倍最近邻放大会有明显台阶）。

用法：python gen-lcd-font.py <lcd_font.h> [lcd_font_big.h]
判据：每行用 `ceil(W/8)` 字节，bit7 = 该字节最左像素。
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

OUT = sys.argv[1] if len(sys.argv) > 1 else "lcd_font.h"
OUT_BIG = sys.argv[2] if len(sys.argv) > 2 else "lcd_font_big.h"

# 小字：全部可打印 ASCII（32..126）
#   ⚠️ 2026-10-25 踩坑（真机/预览同时复现）：第一版只放了当时用到的字符，界面后来加了
#      "TEMP"/"GOOD"/"FAIR"/"WEAK"/"NO NET" ⇒ 缺字的字母被**静默跳过**（"TEMP" 只剩 "T"）。
#      整表 95 字形 ≈ 1.5 KB flash，一次给全，以后改文案不用重烧字体。
CHARS_SMALL = "".join(chr(c) for c in range(32, 127))
# 大字：温度只用到这些
CHARS_BIG = "0123456789.-"

# 🔴 字体选择（真机+预览都验证过）：**等宽粗体 Consolas** 在 8×16 / 16×24 单元格里比比例字体清楚得多
#    —— 比例字体在这个尺寸下 "W" 被压成 "A"、"M" 被压成 "V"、"8" 像 "B"（第一版被用户看出 "WiFi" 像 "AiFi"）。
CANDS = [
    r"C:\Windows\Fonts\consolab.ttf",
    r"C:\Windows\Fonts\consola.ttf",
    r"C:\Windows\Fonts\courbd.ttf",
    r"C:\Windows\Fonts\segoeuib.ttf",
]
font_path = next((c for c in CANDS if os.path.exists(c)), None)
if font_path is None:
    raise SystemExit("找不到可用字体")


def render(chars, W, H, px_size, tail_blank=2):
    """把一组字符渲染成位图行（每行 ceil(W/8) 字节）。"""
    font = ImageFont.truetype(font_path, px_size)
    glyphs = []
    for ch in chars:
        im = Image.new("L", (W, H), 0)
        d = ImageDraw.Draw(im)
        bb = d.textbbox((0, 0), ch, font=font)
        tw, th = bb[2] - bb[0], bb[3] - bb[1]
        d.text(((W - tw) // 2 - bb[0], (H - th) // 2 - bb[1]), ch, fill=255, font=font)
        px = im.load()
        rows = []
        for y in range(H):
            line = bytearray((W + 7) // 8)
            for x in range(W):
                if px[x, y] > 110:
                    line[x // 8] |= (0x80 >> (x % 8))
            rows.extend(line)
        glyphs.append((ch, rows))
    return glyphs


def emit(path, guard, comment, ctype, cname, W, H, chars, px_size):
    glyphs = render(chars, W, H, px_size)
    nbytes = (W + 7) // 8
    lines = [
        f"/* {comment}（自动生成，勿手改：tools/gen-lcd-font.py，字体 consolab） */",
        f"#ifndef {guard}",
        f"#define {guard}",
        "",
        "#include <stdint.h>",
        "",
        f"#define {ctype}_W {W}",
        f"#define {ctype}_H {H}",
        f"#define {ctype}_ROW_BYTES {nbytes}",
        "",
        "typedef struct {",
        "    char    ch;",
        f"    uint8_t rows[{H} * {nbytes}];",
        f"}} {cname}_t;",
        "",
        f"static const {cname}_t k_{cname}[] = {{",
    ]
    for ch, rows in glyphs:
        esc = ch.replace("\\", "\\\\").replace("'", "\\'")
        body = ", ".join("0x%02X" % b for b in rows)
        lines.append("    { '%s', { %s } }," % (esc, body))
    lines += [
        "};",
        "",
        f"#define {ctype}_COUNT (sizeof(k_{cname}) / sizeof(k_{cname}[0]))",
        "",
        f"#endif /* {guard} */",
        "",
    ]
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))
    print("已写 %s（%d 字形，%dx%d，字体 %s）" % (path, len(glyphs), W, H, os.path.basename(font_path)))


emit(OUT, "ONEYE_LCD_FONT_H", "LCD 小字 8x16 位图字体（全可打印 ASCII）", "LCD_FONT", "lcd_glyph",
     8, 16, CHARS_SMALL, 14)
emit(OUT_BIG, "ONEYE_LCD_FONT_BIG_H", "LCD 大字 16x24 位图字体（温度：0-9 . -）", "LCD_FONT_BIG", "lcd_glyph_big",
     16, 24, CHARS_BIG, 26)
