#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把「sdkconfig.defaults / 量产预设里写的配置**真的存在、真的生效**」变成一条可复跑的门禁。

为什么必须有它（2026-09-22/23 两次真机/真构建取证）：
  ① **不存在的符号会被静默忽略**。`sdkconfig.defaults.production` 里曾写
     `CONFIG_ONEYE_FW_TRANSPORT_MQTT_TLS=y`，而 Kconfig 里的符号其实叫
     `CONFIG_ONEYE_FW_TRANSPORT_TLS`（本工程从未有过 `_MQTT_TLS`）⇒ 这一行**什么都没做**，
     "显式钉住承载"成了安慰剂；同一天真机实测到的正是承载掉回 TCP ⇒ 设备完全不上线。
     这类缺陷读代码看不出来：「写了配置」和「配置生效」在文本上长得一模一样。
  ② **"生成后的 sdkconfig"才是唯一判决**。defaults/preset 只在该符号**尚未存在**时被采用；
     只要 sdkconfig 里已经有值，改 defaults 不会自动生效（本仓 sdkconfig 不入库 ⇒ git 里看不见）。

用法（在例程目录或任意位置；`<示例>` = examples/oneye/korvo2_oneye）：
  # ① 不构建也能跑：检查 defaults/preset 里的符号在该工程里是否存在（扫 Kconfig）
  python tools/check-sdkconfig-defaults.py

  # ② 构建后（推荐）：用生成的 sdkconfig 当符号全集，并断言量产预设逐项生效
  python tools/check-sdkconfig-defaults.py \\
      --sdkconfig output/.build/korvo2_oneye-production/sdkconfig \\
      --require sdkconfig.defaults.production

退出码：0 = 通过；1 = 有"不存在的符号"或"预设未生效"；2 = 用法/文件问题。
"""

from __future__ import annotations

import argparse
import os
import pathlib
import re
import sys

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:  # pragma: no cover
    pass

HERE = pathlib.Path(__file__).resolve().parent

# 生成后的 sdkconfig 里，"定义了但没打开"的符号长这样。
NOT_SET = "<not set>"

CONFIG_RE = re.compile(r"^(CONFIG_[A-Za-z0-9_]+)=(.*)$")
NOT_SET_RE = re.compile(r"^# (CONFIG_[A-Za-z0-9_]+) is not set$")
KCONFIG_SYM_RE = re.compile(r"^\s*(?:menu)?config\s+([A-Za-z0-9_]+)\s*$")


def parse_assignments(path: pathlib.Path) -> list[tuple[str, str, int]]:
    """读出 defaults/preset 里的 (符号, 取值, 行号)。

    两种写法都算赋值：`CONFIG_X=v` 与 `# CONFIG_X is not set`（后者是"强制关掉"的规范写法）。
    只按 `=` 解析会把**关掉某项**的那些行整片漏掉 —— 而"漏写/写错一个关闭项"同样会静默失效
    （典型：量产预设里的 `# CONFIG_ONEYE_FW_ENABLE_WIFI_FILE is not set` 若符号名写错，
    明文 Wi-Fi 凭据那条路就悄悄开着）。
    """
    out: list[tuple[str, str, int]] = []
    for n, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        line = raw.strip()
        m = NOT_SET_RE.match(line)
        if m:
            out.append((m.group(1), NOT_SET, n))
            continue
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = (p.strip() for p in line.split("=", 1))
        if key.startswith("CONFIG_"):
            out.append((key, value, n))
    return out


def parse_generated(path: pathlib.Path) -> dict[str, str]:
    """解析生成后的 sdkconfig：`CONFIG_X=v` 与 `# CONFIG_X is not set` 都算"符号存在"。"""
    got: dict[str, str] = {}
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        m = CONFIG_RE.match(line)
        if m:
            got[m.group(1)] = m.group(2)
            continue
        m = NOT_SET_RE.match(line)
        if m:
            got[m.group(1)] = NOT_SET
    return got


def kconfig_symbols(dirs: list[pathlib.Path]) -> set[str]:
    """扫 Kconfig 文件收集符号名（没有构建产物时的退路）。"""
    syms: set[str] = set()
    for root in dirs:
        if not root.is_dir():
            continue
        for path in root.rglob("Kconfig*"):
            if any(part in {".git", "build", "output"} for part in path.parts):
                continue
            try:
                text = path.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            for line in text.splitlines():
                m = KCONFIG_SYM_RE.match(line)
                if m:
                    syms.add("CONFIG_" + m.group(1))
    return syms


def default_kconfig_dirs() -> list[pathlib.Path]:
    """没有 --sdkconfig 时要扫的 Kconfig 根目录。

    三个来源缺一不可：① 例程自身（`main/Kconfig.projbuild`）；② **esp-adf/components**
    （SDK 的 `ONEYE_DEV_CREDS_*` 就落在这里 —— 少了它，量产预设的 `REQUIRED=y` 会被误报成
    "符号不存在"）；③ `$IDF_PATH/components`（IDF 自己的符号）。
    """
    dirs = [HERE.parent]
    adf_components = HERE.parents[3] / "components" if len(HERE.parents) > 3 else None
    if adf_components is not None and adf_components.is_dir():
        dirs.append(adf_components)
    idf = os.environ.get("IDF_PATH")
    if idf:
        dirs.append(pathlib.Path(idf) / "components")
    return dirs


def main() -> int:
    ap = argparse.ArgumentParser(description="校验 sdkconfig.defaults / 量产预设的符号存在性与生效情况")
    ap.add_argument("--dir", default=str(HERE.parent), help="例程目录（含 sdkconfig.defaults*）")
    ap.add_argument("--sdkconfig", default="", help="生成后的 sdkconfig 路径（给了就用它当符号全集并做生效对照）")
    ap.add_argument("--require", action="append", default=[],
                    help="必须逐项生效的 defaults 文件（可重复；常用 = sdkconfig.defaults.production）")
    ap.add_argument("--kconfig-dir", action="append", default=[],
                    help="没有 --sdkconfig 时扫描的 Kconfig 根目录（可重复；缺省 = 例程 + esp-adf/components + $IDF_PATH/components）")
    ap.add_argument("--ignore", action="append", default=[],
                    help="符号名正则白名单（可重复；用于排除环境注入/生成类符号）")
    args = ap.parse_args()

    ex_dir = pathlib.Path(args.dir).resolve()
    if not ex_dir.is_dir():
        print(f"[FAIL] 例程目录不存在：{ex_dir}")
        return 2

    # 要检查的 defaults 文件：显式 --require 的那些 + sdkconfig.defaults*
    check: list[pathlib.Path] = []
    for name in args.require:
        p = pathlib.Path(name)
        if not p.is_absolute():
            p = ex_dir / name
        if not p.is_file():
            print(f"[FAIL] 指定的 defaults 文件不存在：{p}")
            return 2
        check.append(p)
    for p in sorted(ex_dir.glob("sdkconfig.defaults*")):
        if p.is_file() and p not in check:
            check.append(p)

    required = {pathlib.Path(n).name for n in args.require}
    gen: dict[str, str] | None = None
    if args.sdkconfig:
        sp = pathlib.Path(args.sdkconfig)
        if not sp.is_file():
            print(f"[FAIL] --sdkconfig 指向的文件不存在：{sp}")
            return 2
        gen = parse_generated(sp)

    dirs = [pathlib.Path(d).resolve() for d in args.kconfig_dir] or default_kconfig_dirs()
    universe = kconfig_symbols(dirs)    # ① 本工程已有的生成产物（sdkconfig 被 gitignore 但通常就在旁边）是**最贴近事实**的符号全集：
    #    它包含 IDF 与组件管理器注入的符号（CONFIG_IDF_TARGET、CONFIG_ESP32S3_DEFAULT_CPU_FREQ_240…）
    #    以及托管组件的符号 —— 只扫 Kconfig 会把它们误报成"不存在"。
    local_gen = ex_dir / "sdkconfig"
    if gen is None and local_gen.is_file():
        universe |= set(parse_generated(local_gen))
    # ② 显式 --sdkconfig：它同时是"生效对照"的基准。
    if gen is not None:
        universe |= set(gen)
    ignores = [re.compile(p) for p in args.ignore]
    universe = {s for s in universe if not any(r.search(s) for r in ignores)}
    if not universe:
        print("[FAIL] 符号全集为空：既没有 --sdkconfig，也没能扫到任何 Kconfig（用 --kconfig-dir 指定）")
        return 2

    src = pathlib.Path(args.sdkconfig).name if gen is not None else "Kconfig 扫描 + 本地 sdkconfig"
    print(f"符号全集 = {len(universe)} 个（来源：{src}）")
    print(f"检查文件 = {', '.join(p.name for p in check)}")
    if gen is None and not os.environ.get("IDF_PATH"):
        # 诚实标注：没有 IDF 的 Kconfig 可扫时，"符号不存在"里会混进**误报**
        # （IDF 自有/派生符号，例如 CONFIG_LOG_MAXIMUM_LEVEL_INFO）。别让一条会误报的门禁
        # 变成"大家都无视它" —— 要么 source export.sh，要么给 --sdkconfig（推荐）。
        print("[note] 未设置 IDF_PATH：IDF 自有/派生符号无法核对，**可能出现误报**；"
              "建议 `source export.sh` 或改传 --sdkconfig <构建目录>/sdkconfig（构建后最准）")

    failures = 0
    for path in check:
        unknown: list[tuple[str, int]] = []
        ineffective: list[tuple[str, str, str, int]] = []
        for key, value, line in parse_assignments(path):
            if key not in universe:
                unknown.append((key, line))
                continue
            if gen is not None and path.name in required:
                actual = gen.get(key, "<缺失>")
                if actual != value:
                    ineffective.append((key, value, actual, line))
        if unknown:
            failures += len(unknown)
            print(f"\n[FAIL] {path.name}：{len(unknown)} 个符号在本工程里**不存在**（写了也不会生效）")
            for key, line in unknown:
                print(f"       {path.name}:{line}  {key}")
        if ineffective:
            failures += len(ineffective)
            print(f"\n[FAIL] {path.name}：{len(ineffective)} 项**没有真的生效**（生成后的 sdkconfig 对不上）")
            for key, want, got, line in ineffective:
                print(f"       {path.name}:{line}  {key}: 期望 {want} / 实际 {got}")
        if not unknown and not ineffective:
            print(f"[ OK ] {path.name}：{len(parse_assignments(path))} 项全部存在"
                  + ("且已生效" if gen is not None and path.name in required else ""))

    if failures:
        print(f"\nFAIL——共 {failures} 项。"
              f"\n提示：符号名以 Kconfig 里的 `config <名字>` 为准（`CONFIG_` 是它加的前缀）；"
              f"\n      本仓踩过一次：`CONFIG_ONEYE_FW_TRANSPORT_MQTT_TLS` 应为 `CONFIG_ONEYE_FW_TRANSPORT_TLS`。")
        return 1
    print("\nPASS——defaults/预设里的每一项都存在且（在 --require 范围内）已生效")
    return 0


if __name__ == "__main__":
    sys.exit(main())
