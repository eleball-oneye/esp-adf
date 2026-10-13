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

扫描面（🔴 2026-10-13 用户裁定「排除该预设」）：
  **自动挑选的 defaults 文件里排除 `sdkconfig.defaults.esp32s3`** —— 它是目标/板级预设，
  其中 `CONFIG_IDF_TARGET*` / `CONFIG_ESPTOOLPY_FLASHSIZE*` / `CONFIG_SPIRAM*`（8 个符号）
  由 IDF 的**目标机制**注入，⛔ 不在本工程任何 Kconfig 里 ⇒ 按"扫 Kconfig 得符号全集"的口径
  **结构性误报**（永远对不上，不是真缺陷）。
  排除理由与被排除的符号清单**逐字登记在下面的 `SKIP_FILES`**，且每次运行都打印
  `[SKIP] <文件>：<理由>` + 逐条 `被排除的符号：…`（⛔ 不静默）。
  ⛔ 其余 `sdkconfig.defaults*` 一律照旧**真扫**（`sdkconfig.defaults.bench-local` / `.ble` /
  `.ble-key` / `.diag` / `.production` 等）；⛔ 本档内容一字未改。
  想仍然核这一档时，显式传 `--require sdkconfig.defaults.esp32s3`（会给真实读数）。

用法（在例程目录或任意位置；`<示例>` = examples/oneye/korvo2_oneye）：
  # ① 不构建也能跑：检查 defaults/preset 里的符号在该工程里是否存在（扫 Kconfig）
  python tools/check-sdkconfig-defaults.py

  # ② 构建后（推荐）：用生成的 sdkconfig 当符号全集，并断言量产预设逐项生效
  python tools/check-sdkconfig-defaults.py \\
      --sdkconfig output/.build/korvo2_oneye-production/sdkconfig \\
      --require sdkconfig.defaults.production

⚠️ **调用前提（硬性）：必须设 `IDF_PATH`**（🔴 2026-10-13 集成验收实测登记）：
  `default_kconfig_dirs()` 的**第三个来源**就是 `$IDF_PATH/components` 的 Kconfig。缺了它，
  符号全集退化为「例程 Kconfig ＋ esp-adf/components Kconfig ＋ 生成 sdkconfig」，
  ⇒ 预设里那些**由 IDF 声明**的符号（`CONFIG_BT_NIMBLE_*`、`CONFIG_ESP_COEX_*`、
  `CONFIG_LOG_MAXIMUM_LEVEL*` 等）会被判成"本工程不存在" —— 实测**多出 13 条结构性误报**。
  · 正确调用（任选其一）：
      source <esp-idf>/export.sh && python3 tools/check-sdkconfig-defaults.py --sdkconfig <build>/sdkconfig --require <档>
      IDF_PATH=<esp-idf 绝对路径> python3 tools/check-sdkconfig-defaults.py --sdkconfig <build>/sdkconfig --require <档>
      python3 tools/check-sdkconfig-defaults.py --sdkconfig <build>/sdkconfig --require <档> \\
          --kconfig-dir <例程目录> --kconfig-dir <esp-adf>/components --kconfig-dir <esp-idf>/components
      （⛔ 只给 `--kconfig-dir <esp-idf>/components` 会**丢掉**例程与 esp-adf 的 Kconfig ⇒ 换成另一种误报）
  · ⛔ 别把这类误报当缺陷去删预设里的行（删了才会真的丢配置）；缺 IDF_PATH 时下列 `[note]` 会打印提示。
  · 本仓有**两处副本**（`examples/oneye/korvo2_oneye/tools/` 与 `examples/oneye/devkitc_oneye/tools/`），
    前提与判据**逐字相同**（改动须两处同步）。

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


def effective(want: str, got: str) -> bool:
    """判断预设里的取值 `want` 与生成体的取值 `got` 是否**语义等价**。

    🔴 2026-10-13 归一（集成验收 RED-2a）：**bool 的 `=n` ⟺ `# CONFIG_X is not set`**。
    成因：预设里把某个 bool 关掉，人写的是 `CONFIG_X=n`；而 IDF/kconfiglib 生成 sdkconfig 时
    **从不写 `CONFIG_X=n`**，只写 `# CONFIG_X is not set`（本模块把它读成 `NOT_SET`）。
    旧判据做**字面串比较** ⇒ 两者被判"没生效"，而实际取值完全正确（假阳）。
    取证（devkitc beta 档，2026-10-13）：预设 `sdkconfig.defaults.beta:26` 写
    `CONFIG_ONEYE_FW_DEV_CRED_REQUIRED=n`，生成体 `build-beta/sdkconfig:628` 逐字为
    `# CONFIG_ONEYE_FW_DEV_CRED_REQUIRED is not set`。

    ⚠️ **这不是把门禁放宽**，只归一种形态（`n` ↔ `is not set`）：
      · 其它一切取值仍**严格逐字**比较（`y` 必须逐字 `y`；字符串/整数/十六进制一律逐字）；
      · ⛔ `y` 与 `is not set` **不**等价（这是真实失效，必须继续判红）；
      · ⛔ 符号存在性判据（`key not in universe`）一字未动。
    """
    if want == got:
        return True
    return (want == "n" and got == NOT_SET) or (want == NOT_SET and got == "n")

# ── 扫描面排除（🔴 2026-10-13 用户裁定「排除该预设」· 逐字登记，⛔ 不静默）──────
# `sdkconfig.defaults.esp32s3` 是**目标/板级预设**：里面的符号由 **IDF 的目标机制**决定，
# 形态是「IDF 灌进来的当前值」而不是「本工程 Kconfig 里声明的符号」——
# 本工程（例程 + esp-adf/components + esp-sr/ADF 组件）的 Kconfig **没有**这些 `config`，
# 因此"扫 Kconfig 得符号全集"这条口径在这类文件上**结构性地**对不上（永远误报，不是真缺陷）。
# 排除理由与被排除的符号清单**逐字登记在这里**（+运行时会打印 `[SKIP]` 行）：
#   · 目标选择：IDF 的目标机制（`CONFIG_IDF_TARGET` / `CONFIG_IDF_TARGET_ESP32S3`）
#     —— 由 `idf.py set-target` 写进生成后的 sdkconfig，⛔ 不在任何工程 Kconfig 里；
#   · 主频：`CONFIG_ESP32S3_DEFAULT_CPU_FREQ_240` / `_MHZ` —— IDF 的 SoC 组件符号；
#   · Flash 容量：`CONFIG_ESPTOOLPY_FLASHSIZE_8MB` / `CONFIG_ESPTOOLPY_FLASHSIZE` —— IDF 的 esptool 组件符号；
#   · PSRAM：`CONFIG_SPIRAM`（`is not set` 形态）/ `CONFIG_SPIRAM_IGNORE_NOTFOUND` —— IDF 的 esp_psram 组件符号。
# 这不是"把门禁放宽"：本档其余的**本工程**符号仍按同一判据核对（见下面 `check` 的挑选逻辑，
# 显式 `--require sdkconfig.defaults.esp32s3` 仍会真扫它并给出读数）。
SKIP_FILES = {
    "sdkconfig.defaults.esp32s3": (
        "目标/板级预设：以下符号由 IDF 目标机制注入，⛔ 不在本工程 Kconfig（扫 Kconfig 必然误报）",
        [
            "CONFIG_IDF_TARGET",
            "CONFIG_IDF_TARGET_ESP32S3",
            "CONFIG_ESP32S3_DEFAULT_CPU_FREQ_240",
            "CONFIG_ESP32S3_DEFAULT_CPU_FREQ_MHZ",
            "CONFIG_ESPTOOLPY_FLASHSIZE_8MB",
            "CONFIG_ESPTOOLPY_FLASHSIZE",
            "CONFIG_SPIRAM",
            "CONFIG_SPIRAM_IGNORE_NOTFOUND",
        ],
    ),
}

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
    # ⚠️ `required` 必须在打印 `[SKIP]` 之前先算出来（2026-10-13 实跑踩过：放到后面会
    #    在 `--require <被排除档>` 时抛 UnboundLocalError —— 门禁崩了比门禁红了更糟）。
    required = {pathlib.Path(n).name for n in args.require}
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
        if p.is_file() and p not in check and p.name not in SKIP_FILES:
            check.append(p)

    # 排除项**不得静默**：逐个打印 `[SKIP] <文件>：<理由>` + 被排除的符号清单
    # （机械判据 = 每个被排除且存在的文件都有一行含 "SKIP" 的输出）。
    for name, (why, syms) in SKIP_FILES.items():
        p = ex_dir / name
        if not p.is_file():
            continue
        if p in check:
            note = "（本次由 --require 显式指定 ⇒ 仍真扫它）" if name in required else ""
            print(f"[SKIP] {name}：{why}{note}")
        else:
            print(f"[SKIP] {name}：{why}")
        for s in syms:
            print(f"       被排除的符号：{s}")

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
    if not os.environ.get("IDF_PATH"):
        # 诚实标注（🔴 2026-10-13 硬性前提）：没有 IDF 的 Kconfig 可扫时，"符号不存在"里会混进**误报**
        # —— IDF 自有/派生符号（例如 CONFIG_LOG_MAXIMUM_LEVEL_INFO、CONFIG_BT_NIMBLE_*、
        # CONFIG_ESP_COEX_*）。devkitc 实测：同一命令缺 IDF_PATH ⇒ **多出 13 条**这类结构性误报。
        # 别让一条会误报的门禁变成"大家都无视它"，⛔ 也别据此去删预设里那些行。
        print("[note] ⚠️ 未设置 IDF_PATH（**本门禁的硬性调用前提**）：IDF 自有/派生符号无法核对，"
              "**会出现误报**（devkitc 实测多出 13 条）；"
              "请 `source <esp-idf>/export.sh`、或 `IDF_PATH=<esp-idf> python3 …`"
              "（给了 `--sdkconfig` 也**仍需** IDF_PATH）；"
              "或用 `--kconfig-dir` 把三个来源都显式给全：<例程目录>、<esp-adf>/components、<esp-idf>/components")

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
                if not effective(value, actual):
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
