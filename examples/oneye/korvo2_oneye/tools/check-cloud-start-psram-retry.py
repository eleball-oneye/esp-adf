#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check-cloud-start-psram-retry.py —— 结构性门禁：证明本批两件事**落成了形状**（A17）

背景（用户裁定，逐字）：
  「`cloud_start` 任务的栈**一律从内部 RAM 分配**」（本批 = 阻塞级回归修复）
  「`on_wifi_ready()` 的一次性守卫 **改成可重试**（设备首次上云失败后无需重启即可再试）」

🔴 2026-10-13〔本批 · A17a **语义反转**，旧断言逐字留痕，⛔ 不抹〕：
  旧 A17a 逐字为：`A17a 取栈顺序：PSRAM 申请在内部 RAM 申请**之前**（先 PSRAM、失败回落）`，
  其判定实现逐字为「`PSRAM_ALLOC.search(code)` 必须存在 ＋ `INT_ALLOC.search(code)` 必须存在 ＋
  PSRAM 的 `m.start()` 必须小于内部 RAM 的 `m.start()`」。
  它**已作废**：真机上"上云任务栈落 PSRAM"的后果 = 每次上云即 assert 复位
  （`components/spi_flash/cache_utils.c:127` 的 `assert(esp_task_stack_is_sane_cache_disabled())`，
  要求当前任务栈 `esp_ptr_in_dram`），调用链见 `main/korvo2_oneye_main.c` 里
  `cloud_start_stack_alloc()` 上方那段注释。故本批改为断言**必须内部 RAM**。

它**不是**注释声称，而是对 `main/korvo2_oneye_main.c` 的**代码**（先剥注释与字符串字面量）
做机械断言：

  ① 上云任务的**静态栈 + 一律内部 RAM**
    A17a **栈必须落内部 RAM**：`heap_caps_calloc(1u, CLOUD_START_STACK_BYTES, MALLOC_CAP_INTERNAL
         | MALLOC_CAP_8BIT)` 必须在；且 `cloud_start_stack_alloc()` 函数体里**不得**出现
         `MALLOC_CAP_SPIRAM`；且全文件**不得**存在"按 CLOUD_START_STACK_BYTES 从 PSRAM 取栈"的申请
         （回归点）；且新日志串「已从 内部 RAM 取到」必须在、旧日志串「已从 **PSRAM** 取到」必须不在。
         ⚠️ 负向对照会把 PSRAM 那一路**造回去**，A17a **必须报红**。
    A17b 静态创建：`xTaskCreateStaticPinnedToCore(cloud_start_task, …, CLOUD_START_STACK_BYTES, …)`
    A17c 栈量与优先级**未被改动**：宏值逐字为 6144u 与 4；静态创建用同一个栈量/优先级宏
    A17d TCB 只能在内部 RAM（`sizeof(StaticTask_t)` + `MALLOC_CAP_INTERNAL`）
    A17e 回落 = **旧行为**：仍保留 `xTaskCreate(cloud_start_task, …, CLOUD_START_STACK_BYTES, …)`
         （它的栈走 `pvPortMalloc()` ⇒ `portFREERTOS_HEAP_CAPS` 同样只从内部 RAM 取）

  ② 一次性守卫**可重试**（失败可再试；成功之后 ⛔ 不得启动第二条链路）
    A17f 重试发生在**同一个任务**里：`ulTaskNotifyTake` 在 `cloud_start_task()` 内
    A17g 触发边在**就绪事件**上：`xTaskNotifyGive(s_cloud_start_handle)` 在 `on_wifi_ready()` 内
    A17h 唤醒受"**上一轮没起来**"约束（`!s_cloud_start_sdk_up`）⇒ 成功之后不会再触发
    A17i 成功标记只有**一个**写入点，且在 `oneye_start()` 的**成功出口**（早退路径不置位）
    A17j 创建点（静态 + 回落）**恰好 2 处**且都在 `on_wifi_ready()` 内
         （⛔ 不许出现第三条通往 `oneye_start()` 的路径）；重试前有半初始化清理前置

退出码：0 = 全部断言通过；1 = 有断言失败（逐条打出）。
**自带负向对照**：逐条注入已知缺陷，同一套检查**必须逐条报红**；
任一条注入没被抓到 ⇒ 本脚本自己的判据是空转的 ⇒ exit 1。
"""

from __future__ import annotations

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
EXAMPLE = os.path.normpath(os.path.join(HERE, ".."))
MAIN_REL = os.path.join("main", "korvo2_oneye_main.c")

STACK_MACRO = re.compile(r"#define\s+CLOUD_START_STACK_BYTES\s+(\S+)")
PRIO_MACRO = re.compile(r"#define\s+CLOUD_START_TASK_PRIO\s+(\S+)")
# ⚠️ 下面两条 = 旧 A17a 的判定实现，**本批作废**（逐字留痕，⛔ 不抹）；现由 A17a-NEG 反向使用。
PSRAM_ALLOC = re.compile(r"heap_caps_calloc\(\s*1u\s*,\s*CLOUD_START_STACK_BYTES\s*,\s*"
                         r"MALLOC_CAP_SPIRAM\s*\|\s*MALLOC_CAP_8BIT\s*\)")
INT_ALLOC = re.compile(r"heap_caps_calloc\(\s*1u\s*,\s*CLOUD_START_STACK_BYTES\s*,\s*"
                       r"MALLOC_CAP_INTERNAL\s*\|\s*MALLOC_CAP_8BIT\s*\)")
INT_STACK_LOG = "已从 内部 RAM 取到"
PSRAM_STACK_LOG = "已从 **PSRAM** 取到"     # 旧日志串逐字（⛔ 不得再出现在产物里）
TCB_ALLOC = re.compile(r"heap_caps_calloc\(\s*1u\s*,\s*sizeof\(StaticTask_t\)\s*,\s*"
                       r"MALLOC_CAP_INTERNAL\s*\|\s*MALLOC_CAP_8BIT\s*\)")
STATIC_CREATE = re.compile(r"xTaskCreateStaticPinnedToCore\(\s*cloud_start_task\s*,")
PLAIN_CREATE = re.compile(r"(?<![\w])xTaskCreate\(\s*cloud_start_task\s*,")
ANY_CREATE = re.compile(r"(?<![\w])xTaskCreate[A-Za-z]*\(\s*cloud_start_task\s*,")
NOTIFY_TAKE = re.compile(r"ulTaskNotifyTake\s*\(")
NOTIFY_GIVE = re.compile(r"xTaskNotifyGive\s*\(\s*s_cloud_start_handle\s*\)")
SDK_UP_TRUE = re.compile(r"s_cloud_start_sdk_up\s*=\s*true\s*;")
GUARD_UP = re.compile(r"!\s*s_cloud_start_sdk_up")
REINIT = re.compile(r"oneye_dev_base_is_initialized\s*\(\s*\)")


def strip_comments_and_strings(text: str) -> str:
    """把块注释 / 行注释 / 字符串字面量的内容替换成空格（保留换行与长度）。

    为什么必须先剥：本文件里 `xTaskCreate(cloud_start_task, …)` 与 `{`/`}` 都会出现在
    **注释**里（旧值留痕、判据说明）—— 不剥就会把注释当成代码，判据随即失真。
    """
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join(ch if ch == "\n" else " " for ch in text[i:j]))
            i = j
        elif text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif c == '"':
            j = i + 1
            while j < n and text[j] != '"':
                if text[j] == "\\":
                    j += 1
                j += 1
            j = min(j + 1, n)
            out.append("".join(ch if ch == "\n" else " " for ch in text[i:j]))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def line_of(text: str, pos: int) -> int:
    return text[:pos].count("\n") + 1


def strip_comments_only(text: str) -> str:
    """只剥注释、**保留字符串字面量**（换行与长度保持）—— 用于「某条日志串还在不在代码里」。

    为什么必须与 `strip_comments_and_strings()` 并存：
      · 那个函数连字符串字面量一起抹 ⇒ 判不了"这条日志串还在不在"；
      · 而本批把旧日志串**逐字留痕写在注释里**（旧值留痕纪律）⇒ 只按原始文本判会**误报**。
    两个函数各管一半，缺一不可。
    """
    out = []
    i, n = 0, len(text)
    while i < n:
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join(ch if ch == "\n" else " " for ch in text[i:j]))
            i = j
        elif text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def body_of(code: str, name: str):
    """在**已剥注释**的文本上按花括号配平取函数体；返回 (body, 起始行, 结束行) 或 (None, None, None)。"""
    m = re.search(r"(?:^|\n)[ \t]*(?:static\s+)?[A-Za-z_][A-Za-z0-9_ \*]*\b" + re.escape(name) +
                  r"\s*\([^;{]*\)\s*\n?\s*\{", code)
    if not m:
        return None, None, None
    i = code.find("{", m.start())
    depth = 0
    for j in range(i, len(code)):
        if code[j] == "{":
            depth += 1
        elif code[j] == "}":
            depth -= 1
            if depth == 0:
                return code[i:j + 1], line_of(code, m.start()), line_of(code, j)
    return None, None, None


def enclosing(code: str, line: int) -> str:
    spans = []
    for name in ("cloud_start_task", "on_wifi_ready", "oneye_start", "app_main"):
        _b, s, e = body_of(code, name)
        if s is not None and s <= line <= e:
            spans.append((e - s, name))
    if not spans:
        return "?"
    spans.sort()
    return spans[0][1]


def check(raw: str):
    """对**原始文本**做检查（内部先剥注释与字符串）。返回失败列表。"""
    code = strip_comments_and_strings(raw)
    fails: list[str] = []

    def need(cond, msg):
        if not cond:
            fails.append(msg)

    # ---------------- ① 静态栈 + **一律内部 RAM**（本批 A17a 语义反转） ----------------
    # 旧 A17a 逐字留痕（⛔ 不抹）：断言「`PSRAM_ALLOC` 存在 ＋ `INT_ALLOC` 存在 ＋
    #   PSRAM 申请的偏移 < 内部 RAM 申请的偏移」＝「PSRAM 优先、失败回落」。
    # ⇒ 本批作废：真机上"上云任务栈落 PSRAM"= 每次上云即 assert 复位（证据见模块头）。
    alloc_body, _as, _ae = body_of(code, "cloud_start_stack_alloc")
    a_psram, a_int = PSRAM_ALLOC.search(code), INT_ALLOC.search(code)

    need(a_int is not None,
         "A17a 没有看到「按 CLOUD_START_STACK_BYTES 从**内部 RAM**取栈」的申请"
         "（栈必须落内部 RAM：cloud_start 要读 creds 分区 ⇒ flash 关 cache 时 "
         "cache_utils.c:127 的 assert 要求当前任务栈 esp_ptr_in_dram）")
    need(a_psram is None,
         "A17a 🔴 回归：又出现「按 CLOUD_START_STACK_BYTES 从 **PSRAM** 取栈」的申请（第 {} 行）"
         "⇒ 上云任务栈落 PSRAM ⇒ 真机每次上云 assert 复位".format(
             line_of(code, a_psram.start()) if a_psram else -1))
    if alloc_body is None:
        fails.append("A17a 找不到 cloud_start_stack_alloc() 的函数体（取栈落点不可判定）")
    else:
        need("MALLOC_CAP_SPIRAM" not in alloc_body,
             "A17a cloud_start_stack_alloc() 函数体内出现 `MALLOC_CAP_SPIRAM`"
             "⇒ 取栈落点不唯一、含 PSRAM 一路（本批要求**只有**内部 RAM 一路）")
        need(INT_ALLOC.search(alloc_body) is not None,
             "A17a `cloud_start_stack_alloc()` 体内没有按 CLOUD_START_STACK_BYTES 取内部 RAM 的申请")
    # 日志串对照（必须只看**代码里的字符串字面量**：旧串在注释里逐字留痕 ⇒ 不能用原始文本判）
    lit = strip_comments_only(raw)
    need(INT_STACK_LOG in lit,
         "A17a 代码里找不到新日志串「{}」⇒ 取栈落点的运行时自证缺失".format(INT_STACK_LOG))
    need(PSRAM_STACK_LOG not in lit,
         "A17a 代码里仍能找到旧日志串「{}」⇒ 仍在从 PSRAM 取栈".format(PSRAM_STACK_LOG))

    need(STATIC_CREATE.search(code) is not None,
         "A17b 没有 `xTaskCreateStaticPinnedToCore(cloud_start_task, …)`"
         "（缺它 ⇒ 栈缓冲无处传入 ⇒ 仍在用 xTaskCreate 的内部 RAM 栈）")
    sm, pm = STACK_MACRO.search(code), PRIO_MACRO.search(code)
    need(sm is not None and sm.group(1) == "6144u",
         "A17c 栈量宏 `CLOUD_START_STACK_BYTES` 不是逐字的 `6144u`（实测 {}）"
         "—— ⛔ 本批不改栈量语义".format(sm.group(1) if sm else "(缺失)"))
    need(pm is not None and pm.group(1) == "4",
         "A17c 优先级宏 `CLOUD_START_TASK_PRIO` 不是逐字的 `4`（实测 {}）"
         "—— ⛔ 本批不改优先级".format(pm.group(1) if pm else "(缺失)"))
    need(re.search(r"CLOUD_START_STACK_BYTES\s*,\s*NULL\s*,\s*CLOUD_START_TASK_PRIO", code)
         is not None,
         "A17c 静态创建没有用「同一个栈量宏 + 同一个优先级宏」"
         "（形如 …, CLOUD_START_STACK_BYTES, NULL, CLOUD_START_TASK_PRIO, …）")

    need(TCB_ALLOC.search(code) is not None,
         "A17d TCB 不是按 `sizeof(StaticTask_t)` + `MALLOC_CAP_INTERNAL` 申请"
         "（TCB 落 PSRAM 会被内核的 xPortCheckValidTCBMem 拒绝）")

    need(PLAIN_CREATE.search(code) is not None,
         "A17e 找不到回落的 `xTaskCreate(cloud_start_task, …)`（旧行为兜底缺失）")

    # ---------------- ② 可重试 ----------------
    task_body, _ts, _te = body_of(code, "cloud_start_task")
    ready_body, _rs, _re = body_of(code, "on_wifi_ready")
    start_body, _ss, _se = body_of(code, "oneye_start")
    if not task_body:
        fails.append("A17f 找不到 cloud_start_task() 的函数体（重试形状不可判定）")
    if not ready_body:
        fails.append("A17g 找不到 on_wifi_ready() 的函数体（守卫形状不可判定）")
    if not start_body:
        fails.append("A17i 找不到 oneye_start() 的函数体（成功标记不可判定）")

    need(NOTIFY_TAKE.search(task_body) is not None,
         "A17f cloud_start_task() 内没有 `ulTaskNotifyTake(` ⇒ 失败后无处重试"
         "（守卫仍是一次性，首次失败只能重启设备）")
    need(NOTIFY_GIVE.search(ready_body) is not None,
         "A17g on_wifi_ready() 内没有 `xTaskNotifyGive(s_cloud_start_handle)`"
         "⇒ 下一次就绪事件（GOT_IP）不会触发重试")
    need(GUARD_UP.search(ready_body) is not None,
         "A17h 唤醒没有受 `!s_cloud_start_sdk_up` 约束 ⇒ 成功之后仍会发通知"
         "（“重复启动”这条负面判据失去约束）")

    n_sdk_up = len(SDK_UP_TRUE.findall(code))
    need(n_sdk_up == 1,
         "A17i `s_cloud_start_sdk_up = true;` 的写入点实测 {} 处（要求恰好 1）".format(n_sdk_up))
    if n_sdk_up == 1 and start_body:
        need(SDK_UP_TRUE.search(start_body) is not None,
             "A17i 成功标记不在 `oneye_start()` 内 ⇒ 「SDK 起来了」没有单一事实源")
        pos_mark = start_body.find("s_cloud_start_sdk_up = true;")
        pos_ret = start_body.rfind("return;")
        need(pos_ret >= 0, "A17i oneye_start() 内看不到早退 return（形状异常，判据会打空）")
        need(pos_ret < pos_mark,
             "A17i 成功标记落在最后一条早退 `return;`（偏移 {}）之前（标记偏移 {}）"
             "⇒ 失败路径也会被当成「起来了」".format(pos_ret, pos_mark))

    need(REINIT.search(task_body) is not None,
         "A17j 重试前没有 `oneye_dev_base_is_initialized()` 前置（半初始化态会让下一次 "
         "`oneye_dev_base_init()` 直接拿 ERR_STATE ⇒ 永远起不来）")

    creates = [(line_of(code, m.start()), m.group(0).split("(")[0].strip())
               for m in ANY_CREATE.finditer(code)]
    need(len(creates) == 2,
         "A17j `cloud_start_task` 的创建点实测 {} 处（要求恰好 2：静态主路径 + 旧行为回落）"
         .format(len(creates)))
    for ln, _fn in creates:
        fn = enclosing(code, ln)
        need(fn == "on_wifi_ready",
             "A17j 创建点（第 {} 行）不在 on_wifi_ready() 内（实测在 {}）".format(ln, fn))
    return fails


INJECTIONS = [
    ("① 🔴 把内部 RAM 取栈**造回 PSRAM**（重演真机 assert 复位那一步；只换 caps）",
     lambda s: re.sub(r"(heap_caps_calloc\(\s*1u\s*,\s*CLOUD_START_STACK_BYTES\s*,\s*)"
                      r"MALLOC_CAP_INTERNAL", r"\1MALLOC_CAP_SPIRAM", s, count=1),
     "A17a"),
    ("① 🔴 把**旧的两路形状**（PSRAM 优先 ＋ 失败回落内部）逐字造回去",
     lambda s: re.sub(
         r"[ \t]*s_cloud_start_stack = \(StackType_t \*\)heap_caps_calloc\(1u, CLOUD_START_STACK_BYTES,\n"
         r"[ \t]*MALLOC_CAP_INTERNAL \| MALLOC_CAP_8BIT\);\n",
         "        s_cloud_start_stack = (StackType_t *)heap_caps_calloc(1u, CLOUD_START_STACK_BYTES,\n"
         "                                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);\n"
         "        if (s_cloud_start_stack == NULL) {\n"
         "            s_cloud_start_stack = (StackType_t *)heap_caps_calloc(1u, CLOUD_START_STACK_BYTES,\n"
         "                                                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);\n"
         "        }\n",
         s, count=1),
     "A17a"),
    ("① 🔴 把旧日志串「已从 **PSRAM** 取到」写回代码（取栈落点自证被反转）",
     lambda s: s.replace("上云任务栈：已从 内部 RAM 取到", "上云任务栈：已从 **PSRAM** 取到"),
     "A17a"),
    ("① 把静态创建换回 xTaskCreate（栈回到内部 RAM，但丢掉静态落点自证）",
     lambda s: s.replace("xTaskCreateStaticPinnedToCore(cloud_start_task", "xTaskCreate(cloud_start_task"),
     "A17b"),
    ("① 改栈量（6144u → 12288u）",
     lambda s: s.replace("#define CLOUD_START_STACK_BYTES  6144u", "#define CLOUD_START_STACK_BYTES  12288u"),
     "A17c"),
    ("① 改优先级（4 → 5）",
     lambda s: s.replace("#define CLOUD_START_TASK_PRIO    4", "#define CLOUD_START_TASK_PRIO    5"),
     "A17c"),
    ("① 把 TCB 放到 PSRAM",
     lambda s: re.sub(r"(sizeof\(StaticTask_t\),\s*)MALLOC_CAP_INTERNAL", r"\1MALLOC_CAP_SPIRAM", s, count=1),
     "A17d"),
    ("② 删掉失败重试的等待（回到一次性守卫）",
     lambda s: s.replace("(void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CLOUD_START_RETRY_WAIT_MS));", ""),
     "A17f"),
    ("② 删掉就绪事件上的唤醒（失败后没人再触发）",
     lambda s: s.replace("xTaskNotifyGive(s_cloud_start_handle);", "(void)0;"),
     "A17g"),
    ("② 去掉「上一轮没起来」这一条约束（成功之后也会通知）",
     lambda s: s.replace("if (!s_cloud_start_sdk_up && s_cloud_start_handle != NULL) {",
                         "if (s_cloud_start_handle != NULL) {"),
     "A17h"),
    ("② 成功标记也写进失败早退路径（把「没起来」当「起来了」）",
     lambda s: s.replace('        ESP_LOGE(TAG, "base init 失败：%s", oneye_dev_strerror(rc));\n        return;',
                         '        ESP_LOGE(TAG, "base init 失败：%s", oneye_dev_strerror(rc));\n'
                         '        s_cloud_start_sdk_up = true;\n        return;'),
     "A17i"),
    ("② 删掉重试前的半初始化清理（deinit 前置）",
     lambda s: s.replace("if (retry && oneye_dev_base_is_initialized()) {", "if (false) {"),
     "A17j"),
    ("② 再开第三条建链路径（在别处也建一次 cloud_start_task）",
     lambda s: s.replace("static void on_wifi_ready(void)\n{",
                         "static void on_wifi_ready(void)\n{\n    (void)xTaskCreate(cloud_start_task, \"cloud_start2\", 6144, NULL, 4, NULL);"),
     "A17j"),
]


def main() -> int:
    src = os.path.join(EXAMPLE, MAIN_REL)
    if not os.path.isfile(src):
        print("找不到 {}".format(src))
        return 2
    with open(src, encoding="utf-8") as f:
        text = f.read()

    print("检查 {}".format(src))
    fails = check(text)
    for m in fails:
        print("  ✗ " + m)
    print("正向断言（A17a–A17j）：{}".format("PASS" if not fails else "FAIL"))

    bad = 0
    for title, mutate, expect in INJECTIONS:
        mutated = mutate(text)
        if mutated == text:
            print("  ⚠️ 负向对照「{}」未改动任何字节（注入点已漂移）—— 记为失败".format(title))
            bad += 1
            continue
        caught = [m for m in check(mutated) if m.startswith(expect)]
        if caught:
            print("  ✓ 负向对照「{}」被 {} 捕获".format(title, expect))
        else:
            print("  ✗ 负向对照「{}」**未被** {} 捕获（判据空转）".format(title, expect))
            bad += 1

    ok = (not fails) and bad == 0
    print("RESULT={} —— 上云任务静态栈**一律内部 RAM**（A17a–A17e）＋ 失败可重试（A17f–A17j）".format(
        "PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
