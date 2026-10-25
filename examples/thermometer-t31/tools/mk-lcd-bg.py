# -*- coding: utf-8 -*-
"""把一张 PNG 转成 **板载屏背景分区镜像**（烧到 partitions.csv 的 `model` 分区，偏移 0x410000）。

用户 2026-10-25 逐字：「UI背景图在 E:\\Oneye\\logo\\oneye1.png」

🔴 2026-10-25 真机照片订正（⛔ 别改回横屏）：这块屏的可见玻璃是 **135(W) × 240(H) 竖屏**
   （官方 demo 的已知可用口径：hres=135 / vres=240 / 不 swap / mirror(x,y) / gap(52,40)）。
   第一版按 240×135 横屏出图 + swap 画，真机上是「转 90° + 每行错切 + 竖条纹」
   —— 因为 CASET 只有 135 列，喂 240 像素/行会每行折到下一行。
   ⇒ 背景图必须是 **135×240**，与 `board_lcd.c` 的 `LCD_W/LCD_H` 同步（不一致会被 bg_probe 拒收）。

为什么不把背景编进固件（board_lcd.c 头注释同一口径）：
  · C6 **没有 PSRAM**，135×240×2 = 64,800 B 的常驻帧缓冲会把内部 RAM 吃光
    （真机稳态 free_internal 只有 ~12 KB）；
  · ⇒ 背景放 flash 的 `model` 分区（1 MB spiffs，本工程不用），固件按 20 行条带
    `esp_partition_read()` 流式读，常驻 RAM 只要 5.4 KB。

镜像格式（16 B 头 + RGB565 LE 像素，与 board_lcd.c 的 bg_probe() 逐字对应）：
    offset 0   : magic "OYLCDv1"（8 B，含结尾 NUL）
    offset 8   : u16 width   (LE)  = 135
    offset 10  : u16 height  (LE)  = 240
    offset 12  : u8  format  (0 = RGB565 little-endian)
    offset 13  : 3 B 保留（0）
    offset 16  : width*height*2 B 像素，逐行、逐像素 LE

用法：
    python tools/mk-lcd-bg.py "E:/Oneye/logo/oneye1.png" build/lcd-bg.bin
    # 烧写（只写 model 分区，不碰 app / creds）：
    python -m esptool --chip esp32c6 -p COM3 -b 460800 write-flash 0x410000 build/lcd-bg.bin
"""
import struct
import sys

from PIL import Image

try:  # Windows 控制台可能是 GBK：避免脚本以 UnicodeEncodeError 收尾
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

W, H = 135, 240          # 与 board_lcd.c 的 LCD_W/LCD_H 一致；⛔ 改这里必须同步改 C
MAGIC = b"OYLCDv1\x00"
FMT_RGB565_LE = 0

# 背景底色 = 源图主色（logo 的红），logo 没覆盖到的区域用它
BG_COLOR = (215, 13, 25)
# 🔴 2026-10-25 用户要求「背景图的奶油字移到最上方」⇒ 裁掉源图上下/左右的红边，只留 logo 本体，
#    横向铺满 118 px（左右各 8 px 边距）、贴顶部 y=3 放置；其余区域留红底给固件画卡片。
LOGO_W = 118
LOGO_TOP = 3
LOGO_MAX_H = 46          # 顶部区域高度上限（下面是卡片区）


def trim_to_logo(im):
    """把源图裁剪到「奶油字/图标」的包围盒（源图四周是大片红底，直接缩放会让 logo 变小）。"""
    px = im.load()
    w, h = im.size
    minx, miny, maxx, maxy = w, h, -1, -1
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            if abs(r - BG_COLOR[0]) > 60 or abs(g - BG_COLOR[1]) > 60 or abs(b - BG_COLOR[2]) > 60:
                if x < minx:
                    minx = x
                if x > maxx:
                    maxx = x
                if y < miny:
                    miny = y
                if y > maxy:
                    maxy = y
    if maxx < 0:
        return im
    return im.crop((minx, miny, maxx + 1, maxy + 1))


def build(src_png: str, out_bin: str) -> None:
    im = trim_to_logo(Image.open(src_png).convert("RGB"))

    canvas = Image.new("RGB", (W, H), BG_COLOR)

    # logo：按宽度铺满 LOGO_W（等比，不拉伸），水平居中、贴顶
    scale = LOGO_W / im.width
    nw, nh = LOGO_W, max(1, int(im.height * scale + 0.5))
    if nh > LOGO_MAX_H:                     # 太高就按高度收，保持清晰
        nw, nh = max(1, int(nw * LOGO_MAX_H / nh + 0.5)), LOGO_MAX_H
    logo = im.resize((nw, nh), Image.LANCZOS)
    canvas.paste(logo, ((W - nw) // 2, LOGO_TOP))

    px = canvas.load()
    buf = bytearray()
    for y in range(H):
        for x in range(W):
            r, g, b = px[x, y]
            v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            buf += bytes((v & 0xFF, (v >> 8) & 0xFF))     # RGB565 little-endian

    header = MAGIC + struct.pack("<HHB", W, H, FMT_RGB565_LE) + b"\x00\x00\x00"
    assert len(header) == 16, len(header)
    with open(out_bin, "wb") as f:
        f.write(header)
        f.write(buf)

    total = len(header) + len(buf)
    # 顺手落一张 135×240 的 PNG 供 tools/preview-lcd.py 对照（不参与烧写）
    try:
        canvas.save(out_bin + ".png")
    except Exception:
        pass
    print(f"已生成 {out_bin}: {total} B（头 16 B + {W}x{H} RGB565 = {len(buf)} B，logo {nw}x{nh}）")
    print(f"烧写：python -m esptool --chip esp32c6 -p COM3 -b 460800 write-flash 0x410000 {out_bin}")
    print("⚠️ model 分区 1 MB，本镜像 %d B ⇒ 余量充足；⛔ 不要写到 0x510000（那是 creds 分区）" % total)


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(__doc__)
        raise SystemExit(2)
    build(sys.argv[1], sys.argv[2])
