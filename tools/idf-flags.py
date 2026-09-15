#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
oneye-dev-sdk · 从 ESP-IDF 自身生成的 compile_commands.json 提取"真实编译参数"。

为什么需要它（背景，勿删）：
  build-all.sh 的 ESP 轨用独立交叉编译把 SDK 编成 4 个 `.a`。若在这里**手工枚举** IDF 的
  头文件与宏，实测会一路踩坑：sdkconfig.h → FreeRTOSConfig.h → FreeRTOSConfig_arch.h →
  portmacro.h（`-std=c99` 会关掉 asm）→ esp_newlib.h，通配 `components/*/include` 还会把
  **别的芯片**的 soc 头（如 esp32c2）抢先命中。因此改为：让 IDF 自己配置一个一次性探针工程，
  再从它产出的 compile_commands.json 汇总 `-I/-D/-std/-m*`，按工具链缓存复用。

输出格式：**每行一个编译参数**（不做引号转义）。调用方（bash）按行读入数组后用
`"${arr[@]}"` 展开，参数边界原样保留 —— 这是有意为之：像
`-DMBEDTLS_CONFIG_FILE="mbedtls/esp_config.h"` 这类含引号的参数若写成 GCC `@file`
响应文件，会被 GCC 的引号解析规则吃掉引号（宏值变成非字符串字面量而编译失败）。

用法：
  idf-flags.py --compile-commands <cc.json> --out <flags.txt> [--source <路径片段>]

`--source` 用于优先挑选"探针组件自己的那条编译记录"（其 include 集合正是该组件
REQUIRES 求交后的结果，最贴近 SDK 组件的真实待遇）；找不到则退化为全表并集。
"""

import argparse
import json
import sys

# 只保留这些参数：头文件搜索路径、宏、语言标准、架构（-m*）。
# 其余（-c/-o/依赖生成/优化等级/告警开关）一律丢弃：优化与告警由 build-all.sh 自己决定。
_KEEP_PREFIX = ("-I", "-D", "-U")

# 丢弃"描述这次调用本身"的参数：编译/链接动作、依赖文件生成、诊断着色、
# 以及 IDF 的告警与优化/调试等级（我们自带 -Wall -Wextra -O2）。
_DROP_EXACT = {
    "-c", "-o", "-MD", "-MMD", "-MP", "-MF", "-MT", "-MQ",
    "-Wall", "-Wextra",       # 告警等级由 build-all.sh 决定
    "-Werror",                # 不继承 IDF 的"告警即错误"
}
_DROP_PREFIX = (
    "-Werror=",
    "-Wno-error=",            # 与 -Werror 配套的例外项；没有 -Werror 时无意义
    "-O",                     # 优化等级由 build-all.sh 决定
    "-g",                     # 调试信息等级同上
    "-fdiagnostics-color",
    "-fmacro-prefix-map=",    # 会把探针工程路径改写进 __FILE__，对我们无意义
    "-fdebug-prefix-map=",
    "-frandom-seed=",
)
# 注意：**保留** IDF 的 `-Wno-*`（抑制项）。它们是 IDF 针对自身头文件写的，
# 丢掉会让 IDF 头（如 esp_hw_support/include/spinlock.h 的 unused parameter）
# 在我们的构建里刷告警，淹没真正的告警。实测：不保留时每次构建多出 3 条此类噪声。


def _split_response(text):
    """按 shell 规则切分响应文件内容（支持 "..." 引号）。"""
    out = []
    cur = []
    in_quote = False
    for ch in text:
        if ch == '"':
            in_quote = not in_quote
            continue
        if not in_quote and ch in " \t\r\n":
            if cur:
                out.append("".join(cur))
                cur = []
            continue
        cur.append(ch)
    if cur:
        out.append("".join(cur))
    return out


def _expand_response_files(args, depth=0):
    """展开 `@<file>` 响应文件（IDF 6 把关键开关放在 build/toolchain/cflags 里，
    例如 -specs=.../picolibc.specs —— 不展开会退回工具链自带 newlib 头，
    与 IDF 的 esp_libc/platform_include 冲突，实测报 __FILE/_REENT 未定义）。"""
    if depth > 3:
        return list(args)
    out = []
    for a in args:
        if a.startswith("@") and len(a) > 1:
            path = a[1:].strip('"')
            try:
                with open(path, "r", encoding="utf-8") as fh:
                    inner = _split_response(fh.read())
            except OSError:
                continue  # 读不到就跳过（不致命）
            out += _expand_response_files(inner, depth + 1)
            continue
        out.append(a)
    return out


def _is_dropped(a):
    if a in _DROP_EXACT:
        return True
    for p in _DROP_PREFIX:
        if a.startswith(p):
            return True
    return False


def _collect(args):
    """从一条编译命令行里挑出我们关心的参数（保持原顺序）。

    口径：**排除法**（丢弃调用自身/告警/优化类），而不是白名单 —— IDF 6 需要
    `-specs=…/picolibc.specs`、`-fno-builtin-memcpy`、`-m*` 等才能与 IDF 头文件自洽，
    白名单漏一个就会编译失败或静默用错 C 库。
    """
    incs, defs, others, std = [], [], [], None
    i = 0
    n = len(args)
    while i < n:
        a = args[i]
        if a in ("-o", "-MF", "-MT", "-MQ") and i + 1 < n:
            i += 2  # 连带它的参数一起丢
            continue
        if _is_dropped(a):
            i += 1
            continue
        if a == "-isystem" and i + 1 < n:  # -isystem <dir> 两段式 → 归入 -I（顺序保留）
            incs.append("-I" + args[i + 1])
            i += 2
            continue
        if a.startswith("-isystem"):  # -isystem<dir> 连写
            incs.append("-I" + a[len("-isystem"):])
            i += 1
            continue
        if a.startswith(_KEEP_PREFIX):
            if a.startswith("-I"):
                incs.append(a)
            else:
                defs.append(a)
            i += 1
            continue
        if a.startswith("-std="):
            std = a
            i += 1
            continue
        if a.startswith("-") :
            others.append(a)  # -m*/-f*/-specs=… 等
            i += 1
            continue
        i += 1  # 源码路径等位置参数
    return incs, defs, others, std


def _dedup(seq):
    seen = set()
    out = []
    for x in seq:
        if x not in seen:
            seen.add(x)
            out.append(x)
    return out


def _unescape(s):
    """还原 compile_commands.json 里的 shell 转义。

    实测必踩：IDF 的条目里宏是 `-DIDF_VER=\\"v5.5.5\\"`、`-DMBEDTLS_CONFIG_FILE=\\"mbedtls/esp_config.h\\"`
    （JSON 解码后仍带反斜杠）。若原样作为单个 argv 传给 GCC，它会看到 `\\"` 并报
    `missing terminating " character`，宏值随之非法 →
    `mbedtls/build_info.h: #include expects "FILENAME" or <FILENAME>`，
    进而 mbedtls_x509_*/mbedtls_ssl_*/mbedtls_pk_* 全部"未声明"。
    这里只还原 `\\"`、`\\\\`、`\\'` 三种转义，其余反斜杠原样保留。
    """
    out = []
    i = 0
    n = len(s)
    while i < n:
        if s[i] == "\\" and i + 1 < n and s[i + 1] in ('"', "\\", "'"):
            out.append(s[i + 1])
            i += 2
            continue
        out.append(s[i])
        i += 1
    return "".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--compile-commands", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--source", default="")
    args = ap.parse_args()

    try:
        with open(args.compile_commands, "r", encoding="utf-8") as fh:
            db = json.load(fh)
    except Exception as exc:  # noqa: BLE001 - 明确报错即可
        print("读取 compile_commands.json 失败：%s" % exc, file=sys.stderr)
        return 2

    if not isinstance(db, list) or not db:
        print("compile_commands.json 为空或格式异常", file=sys.stderr)
        return 2

    entries = []
    if args.source:
        entries = [e for e in db if args.source in (e.get("file") or "")]
    used_fallback = False
    if not entries:
        entries = db
        used_fallback = True

    incs, defs, others, std = [], [], [], None
    for e in entries:
        cmd = e.get("arguments")
        if not cmd:  # 兼容只给 command 字符串的 CMake 版本
            cmd = (e.get("command") or "").split()
        cmd = _expand_response_files(cmd)
        ci, cd, co, cs = _collect(cmd)
        incs += ci
        defs += cd
        others += co
        if cs:
            std = cs

    incs = _dedup(incs)
    defs = _dedup(defs)
    others = _dedup(others)

    if not incs:
        print("未能从 compile_commands.json 提取到任何 -I", file=sys.stderr)
        return 3

    flags = []
    if std:
        flags.append(std)
    flags += defs
    flags += incs
    flags += others
    flags = [_unescape(f) for f in flags]

    with open(args.out, "w", encoding="utf-8", newline="\n") as fh:
        for f in flags:
            fh.write(f + "\n")

    has_esp_platform = "-DESP_PLATFORM" in defs
    # 关键：IDF 6 走 picolibc，靠 -specs=…/picolibc.specs 选择 C 库；丢了它就会退回
    # 工具链自带 newlib 头并与 esp_libc/platform_include 冲突（实测 __FILE/_REENT 未定义）。
    has_specs = any(f.startswith("-specs=") or f.startswith("--specs=") for f in flags)
    print(
        "std=%s -I=%d -D/-U=%d 其他=%d specs=%s ESP_PLATFORM=%s%s"
        % (
            std or "(无)",
            len(incs),
            len(defs),
            len(others),
            "yes" if has_specs else "no",
            "yes" if has_esp_platform else "NO",
            "（未命中 --source，已用全表并集）" if used_fallback else "",
        )
    )
    if not has_esp_platform:
        # 没有 ESP_PLATFORM 就等于没修好本缺陷，直接判失败，避免又产出"能编译但链不上"的库。
        print("错误：提取到的宏里没有 -DESP_PLATFORM", file=sys.stderr)
        return 4
    return 0


if __name__ == "__main__":
    sys.exit(main())
