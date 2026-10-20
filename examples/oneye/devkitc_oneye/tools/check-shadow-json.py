#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# =============================================================================
# check-shadow-json.py —— 影子 `reported` JSON 的**构建期钉子**（2026-10-20 新增）
#
# 为什么需要它（真机缺陷留痕，⛔ 不是"加个检查图安心"）：
#   `main/devkitc_oneye_main.c` 里 `oneye_start()` 的 `char shadow[192]` 是
#   `oneye_dev_base_report_state()` 的**唯一**输入。旧拼装产出（实测形状）：
#       {"firmwareVersion":"…","power":true,"credSource":"credSource":"embedded"}}
#   ⇒ **重复键 ＋ 多一个右括号** ⇒ SDK 判 INVALID **静默失败** ⇒ 平台侧永远看不到 `credSource`。
#   这个缺陷**编译得过、跑得起来**，串口上还可能一行错都不打 ⇒ 只能靠机械校验拦。
#
# 判据（正向 ＋ 两个负向，缺一条这个脚本就等于没写）：
#   ① 从 C 源里取出**真正被编译**的那条 `snprintf(shadow, sizeof(shadow), "<fmt>", …)` 格式串；
#   ② 按 C 的转义规则解回 JSON 文本，把里面的 `%s` 依次替换成**样本值**；
#   ③ `json.loads()` 必须成功，且必须含 `firmwareVersion` / `power` / `credSource` 三键；
#   ④ 用 `object_pairs_hook` 拒绝**重复键**（Python 默认"后者覆盖"会把旧缺陷放过去）；
#   ⑤ 尾随多余字符（多余 `}`）由 `json.loads` 自己报 `Extra data` ⇒ 红。
#
# ⚠️ 射程与边界（如实登记）：本脚本**只**校验"拼装模板"这一个字符串，⛔ 不校验
#   服务端影子键白名单、⛔ 不校验 `credSource` 取了哪个枚举值（那是 `oneye_dev_sdk` 的判据）。
# ⚠️ 若拼装点被改写（换函数 / 换变量名），本脚本会以"找不到模板"**红掉**——
#   那是**期望行为**（⛔ 不许静默跳过：改了拼装就得同步改这个钉子）。
#
# 用法：python3 tools/check-shadow-json.py [<main.c 路径>]
# 退出码：0 = 通过；1 = 不可解析 / 结构不符；2 = 找不到模板（源码被改写）。
# =============================================================================

import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT_SRC = HERE.parent / "main" / "devkitc_oneye_main.c"

# `snprintf(shadow, sizeof(shadow), "<fmt>" …)` —— DOTALL 允许跨行（当前实现就是跨行的）
SNAPSHOT_RE = re.compile(
    r'snprintf\s*\(\s*shadow\s*,\s*sizeof\s*\(\s*shadow\s*\)\s*,\s*"((?:[^"\\]|\\.)*)"',
    re.DOTALL,
)

# 与 `oneye_dev_creds_state_json(ONEYE_DEV_CREDS_SOURCE_EMBEDDED)` 的**实际产物**同形：
#   `{"credSource":"embedded"}` ⇒ 拼装点剥掉开头 `{` 后的内层就是这个串（含自带收尾 `}`）。
CRED_INNER = '"credSource":"embedded"}'
FW_VERSION = "1.0.0"


def c_unescape(s: str) -> str:
    """C 字符串字面量 → 真实字节串（只处理本模板会用到的转义）。"""
    out = []
    i = 0
    while i < len(s):
        ch = s[i]
        if ch != "\\":
            out.append(ch)
            i += 1
            continue
        i += 1
        if i >= len(s):
            raise ValueError("格式串以孤立反斜杠结尾")
        esc = s[i]
        i += 1
        mapping = {"n": "\n", "t": "\t", "r": "\r", '"': '"', "\\": "\\", "'": "'", "0": "\0"}
        if esc in mapping:
            out.append(mapping[esc])
        else:
            # 未知转义：按"字面保留"处理，让后面的 JSON 解析去判红（⛔ 不在这里猜语义）
            out.append("\\" + esc)
    return "".join(out)


def no_duplicate_keys(pairs):
    seen = set()
    for key, _value in pairs:
        if key in seen:
            raise ValueError(f"重复键：{key!r}（同一对象里出现两次 ⇒ 服务端/解析器行为不确定）")
        seen.add(key)
    return dict(pairs)


def main() -> int:
    src = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_SRC
    if not src.is_file():
        print(f"[shadow-json] 找不到源文件：{src}", file=sys.stderr)
        return 2
    text = src.read_text(encoding="utf-8")

    # ⚠️ 注释里也**逐字留痕**了旧拼装（`…snprintf(shadow, …`）⇒ ⛔ 不能用"第一个匹配"：
    #   只认**行首只有空白**的那一处（= 真被编译的代码）。注释行以 `*` / `` ` `` 开头，会被跳过。
    candidates = []
    for m in SNAPSHOT_RE.finditer(text):
        line_start = text.rfind("\n", 0, m.start()) + 1
        if text[line_start:m.start()].strip() == "":
            candidates.append(m)

    if not candidates:
        print(
            "[shadow-json] 找不到**代码行**上的 `snprintf(shadow, sizeof(shadow), \"…\")` 拼装模板"
            " （只找到注释里的留痕？）⇒ 源码被改写过，请同步更新本钉子（⛔ 不许静默跳过）",
            file=sys.stderr,
        )
        return 2
    if len(candidates) > 1:
        print(
            f"[shadow-json] 代码行上找到 {len(candidates)} 处拼装模板（期望 1）⇒ 存在第二事实源，"
            "请先合并成一处",
            file=sys.stderr,
        )
        return 1
    match = candidates[0]

    c_fmt = match.group(1)
    fmt = c_unescape(c_fmt)
    n_ph = fmt.count("%s")
    if n_ph != 2:
        print(
            f"[shadow-json] 格式串里 `%s` 的个数 = {n_ph}（期望 2：firmwareVersion ＋ 凭据内层）"
            f" ⇒ 结构已变：{fmt!r}",
            file=sys.stderr,
        )
        return 1

    rendered = fmt % (FW_VERSION, CRED_INNER)
    try:
        parsed = json.loads(rendered, object_pairs_hook=no_duplicate_keys)
    except Exception as exc:  # noqa: BLE001 —— 这里就是要"任何解析失败都判红"
        print(f"[shadow-json] 影子 JSON **不可解析**：{exc}\n  渲染结果：{rendered}", file=sys.stderr)
        return 1

    required = {"firmwareVersion", "power", "credSource"}
    missing = required - set(parsed)
    if missing:
        print(f"[shadow-json] 缺少必需键：{sorted(missing)}\n  渲染结果：{rendered}", file=sys.stderr)
        return 1
    if parsed["credSource"] != "embedded":
        print(f"[shadow-json] credSource 取值异常：{parsed['credSource']!r}（样本应为 'embedded'）", file=sys.stderr)
        return 1

    print(f"[shadow-json] OK：{rendered}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
