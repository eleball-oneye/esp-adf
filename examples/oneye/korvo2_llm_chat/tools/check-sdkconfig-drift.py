#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""对照 `sdkconfig.defaults`（跟踪文件）与生成出来的 `sdkconfig`（被 .gitignore 忽略），列出**漂移项**。

为什么必须有它（真机取证 2026-09-17）：
  ESP-IDF 只在 `sdkconfig` 里**缺少**某符号时才采用 `sdkconfig.defaults`；一旦生成过一次，
  之后改 defaults **不会**自动生效。而本仓 `sdkconfig` 不入库 ⇒ "defaults 里写了、实际没生效"
  在 git 里完全看不见。当天就踩到：defaults 写 `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`，
  实际是 `is not set` ⇒ WiFi/lwIP 缓冲全落**内部 RAM**，内部余量从 42 KB 掉到 35 KB，
  AFE 的音频线程创建失败（`AUDIO_THREAD: Error creating RestrictedPinnedToCore algo_fetch`
  → `voice_io: 采集任务创建失败`），表现为**长按 REC 也收不到音**。修掉漂移后内部余回升、采集正常。

用法（在本例程目录或任意位置）：
  python tools/check-sdkconfig-drift.py            # 全量漂移清单（有漂移时退出码 1）
  python tools/check-sdkconfig-drift.py --memory   # 只看内存/网络/BLE/AFE 相关
  python tools/check-sdkconfig-drift.py --dir <例程目录>

⚠️ 看到漂移**不要**直接删 `sdkconfig` 重生成：本机有几项是**有意覆盖**且与已烧录的 flash 布局绑定
   （例如 `CONFIG_BOOTLOADER_OFFSET_IN_FLASH`：defaults 写 0x1000、实际 0x0，重生成会导致启动区错位）。
   逐项判断：要么改 `sdkconfig.defaults` 承认现状，要么用 idf.py menuconfig/脚本把该项对齐。
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:  # pragma: no cover
    pass

MEM_KEYS = ("SPIRAM", "LWIP", "BT_NIMBLE", "WIFI", "AFE", "SR_", "ESP_WS", "MALLOC", "HEAP")


def parse_defaults(path: pathlib.Path) -> list[tuple[str, str]]:
    out: list[tuple[str, str]] = []
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, v = (p.strip() for p in line.split("=", 1))
        out.append((k, v))
    return out


def parse_generated(path: pathlib.Path) -> dict[str, str]:
    got: dict[str, str] = {}
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        m = re.match(r"^(CONFIG_[A-Za-z0-9_]+)=(.*)$", line)
        if m:
            got[m.group(1)] = m.group(2)
            continue
        m = re.match(r"^# (CONFIG_[A-Za-z0-9_]+) is not set$", line)
        if m:
            got[m.group(1)] = "<not set>"
    return got


def expected(v: str) -> str:
    """IDF 在 sdkconfig 里的写法：bool → y / `# X is not set`；int → 裸数字；字符串 → 带引号。"""
    if v in ("y", "n"):
        return v
    if re.fullmatch(r"-?\d+", v):
        return v
    if len(v) >= 2 and v.startswith('"') and v.endswith('"'):
        return v  # 已经是带引号的字符串，别再套一层（首版这里套了两层 → 假漂移）
    return f'"{v}"'


def main() -> int:
    here = pathlib.Path(__file__).resolve().parent.parent  # 例程目录
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default=str(here), help="例程目录（含 sdkconfig.defaults 与 sdkconfig）")
    ap.add_argument("--memory", action="store_true", help="只列内存/网络/BLE/AFE 相关漂移")
    args = ap.parse_args()

    cfg = pathlib.Path(args.dir)
    defaults_p, gen_p = cfg / "sdkconfig.defaults", cfg / "sdkconfig"
    if not defaults_p.is_file() or not gen_p.is_file():
        print(f"缺文件：{defaults_p} 或 {gen_p}（先跑一次 idf.py build 生成 sdkconfig）", file=sys.stderr)
        return 2

    drift, missing = [], []
    generated = parse_generated(gen_p)
    for k, v in parse_defaults(defaults_p):
        if k not in generated:
            missing.append((k, v))
            continue
        want, got = expected(v), generated[k]
        # `n` 与 `# CONFIG_X is not set` 是同一件事（IDF 的两种写法），别当漂移
        if want == "n" and got == "<not set>":
            continue
        if want != got:
            drift.append((k, want, got))

    print(f"defaults 条目 {len(parse_defaults(defaults_p))} 条；sdkconfig 条目 {len(generated)} 条")
    if args.memory:
        drift = [d for d in drift if any(k in d[0] for k in MEM_KEYS)]
        print(f"（只列内存/网络/BLE/AFE 相关：{len(drift)} 条）")
    print(f"漂移：{len(drift)} 条；defaults 有而 sdkconfig 无该符号：{len(missing)} 条（IDF 会用 defaults 值，通常无害）")
    for k, want, got in drift:
        print(f"  {k}: defaults={want} 实际={got}")
    if missing:
        for k, v in missing[:20]:
            print(f"  （缺符号）{k}={v}")
    return 0 if not drift else 1


if __name__ == "__main__":
    sys.exit(main())
