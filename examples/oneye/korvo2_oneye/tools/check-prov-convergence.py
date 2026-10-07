#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
check-prov-convergence.py —— 结构性门禁：证明 korvo2_oneye 的「入网后收敛点」唯一
（ADR-0017 D2）**且**「通道优先级 / 只入网一次」可判定（ADR-0017 D2/D4）。

它**不是**注释声称，而是对 `main/*.c`、两个关键头文件与主机侧夹具做机械断言。
被断言的形状（2026-10-07 定稿）：

    wifi_prov.c : IP_EVENT_STA_GOT_IP 分支
        └─ s_ready_cb()                       ← 唯一触发点
             └─ main.c : on_wifi_ready()      ← 唯一注册点（含一次性守卫 ⇒ 只入网一次）
                  └─ xTaskCreate(cloud_start_task, ...)   ← 唯一创建点
                       └─ oneye_start()       ← 唯一调用点

    所有通道（C1/C2 BLE、C3 凭据文件、C4 台面 API、C5 Kconfig 兜底）
        └─ wifi_prov_submit_credentials*()    ← 唯一入网入口
             └─ wifi_prov_policy_decide()     ← 唯一判据（SD rank0 > BLE rank1 > 其他 rank2）
                  ├─ 采信 ⇒ wifi_prov_connect()（**static**，模块外拿不到 ⇒ 判据不可旁路）
                  └─ 拒绝 ⇒ 留痕 + ESP_ERR_INVALID_STATE（不发起入网）

断言清单：
  A1–A7  单一收敛点（oneye_start()/cloud_start_task/ready 回调 的调用点·创建点·注册点·触发点各唯一）
  A8     优先级可判定：SD 先于 BLE 被递上去、连接层不可旁路、通道全部经唯一入口、五种决策都有落点
  A9     只入网一次：on_wifi_ready 一次性守卫、判据 `onboarded = s_connected && s_held_valid`、
         判据顺序（onboarded 先于 held_valid）、释放/重置都会清采信槽
  A10    夹具与判据**同源**（不抄件）且**自带变异负向对照**，并实跑一次（有 cc 时）
  A11    入网后的**启动期自检与授时**（net_probe_report / sntp_boot_sync）各只有 1 处调用点，
         且都在 korvo2_oneye_main.c 的 on_wifi_ready() 内（= 收敛点下游）⇒ **所有通道**都经过它们；
         通道模块（wifi_prov_ble.c / media_api.c / wifi_prov.c / panel_api.c …）里不得出现这两个符号
  A12    收敛点下游的**顺序**：net_probe → sntp → 面板 → `xTaskCreate(cloud_start_task,…)`。
         ⚠️ sntp 必须在 cloud_start_task 之前：严格 TLS 校验需要先把时钟校好（设备无 RTC）

退出码：0 = 全部断言通过；1 = 有断言失败（逐条打出）。
**自带负向对照**：把 `main/` 与夹具复制到临时目录后逐条注入已知缺陷，同一套检查**必须逐条报红**
（含 A1 的第二条入网后逻辑、A8b 的直连连接层、A9b 的丢掉"只入网一次"、A8e 的留痕缺失、
A10b 的判据抄件、**A11 的"通道模块里再调一次 sntp_boot_sync()/net_probe_report()"**、
**A12 的"把 sntp 挪到 cloud_start_task 之后"**）。任一条注入没有被对应断言抓住 ⇒ 本脚本自己的
判据是空转的 ⇒ exit 1。
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
EXAMPLE = os.path.normpath(os.path.join(HERE, ".."))
MAIN_REL = "main"
TOOLS_REL = "tools"
TEST_REL = os.path.join(TOOLS_REL, "test-prov-priority.c")

CALL_ONEYE_START = re.compile(r"oneye_start\s*\(\s*\)\s*;")
CREATE_CLOUD_TASK = re.compile(r"xTaskCreate\s*\(\s*cloud_start_task\b")
REG_READY_CB = re.compile(r"wifi_prov_set_ready_cb\s*\(\s*on_wifi_ready\s*\)")
CALL_READY_CB = re.compile(r"s_ready_cb\s*\(\s*\)\s*;")
SUBMIT_CREDS = re.compile(r"wifi_prov_submit_credentials\s*\(")
SUBMIT_CREDS_SRC = re.compile(r"wifi_prov_submit_credentials_src\s*\(")
CONNECT_CALL = re.compile(r"wifi_prov_connect\s*\(")
SET_SOURCE_CALL = re.compile(r"wifi_prov_set_source\s*\(")
CHAN_BLE = re.compile(r"WIFI_PROV_CHAN_BLE")
GOT_IP = re.compile(r"IP_EVENT_STA_GOT_IP")
LOAD_FILE_CALL = re.compile(r"wifi_prov_load_file\s*\(")
BLE_START_CALL = re.compile(r"wifi_prov_ble_start\s*\(")
RELEASE_HELD_CALL = re.compile(r"wifi_prov_release_held\s*\(")
# A11/A12：入网后的启动期自检与授时（2026-10-07 起必须只在收敛点下游）
CALL_NET_PROBE = re.compile(r"net_probe_report\s*\(")
CALL_SNTP_BOOT = re.compile(r"sntp_boot_sync\s*\(")
CALL_PANEL_START = re.compile(r"panel_start_if_enabled\s*\(")
# 通道模块（判据侧只允许"交凭据"；出现上面两个符号即第二条入网后路径）
CHANNEL_MODULES = ("wifi_prov_ble.c", "media_api.c", "wifi_prov.c", "panel_api.c",
                   "wifi_prov_file.c", "wifi_prov_sonic.c", "wifi_prov_qrcode.c")
DECISION_ENUM = [
    "WIFI_PROV_DECISION_ACCEPT_FIRST",
    "WIFI_PROV_DECISION_ACCEPT_SAME",
    "WIFI_PROV_DECISION_ACCEPT_PREEMPT",
    "WIFI_PROV_DECISION_REJECT_LOWER",
    "WIFI_PROV_DECISION_REJECT_ALREADY_UP",
]
# 夹具里"抄一份判据"就会撞上的名字（A10b）
JUDGE_NAMES = [
    "wifi_prov_policy_decide",
    "wifi_prov_channel_rank",
    "wifi_prov_decision_is_accept",
    "wifi_prov_decision_str",
]


def read_lines(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.read().splitlines()


def read_sources(root):
    """{文件名: 行列表}; 只看 main/*.c（.h 是声明面，不构成本收敛点的可执行路径）"""
    out = {}
    main = os.path.join(root, MAIN_REL)
    for fn in sorted(os.listdir(main)):
        if fn.endswith(".c"):
            out[fn] = read_lines(os.path.join(main, fn))
    return out


def enclosing_function(lines, idx):
    """向上找最近的函数定义行（`<...>name(args)` 且该行以 ')' 结尾、下一行是 '{'）"""
    for i in range(idx, -1, -1):
        s = lines[i].strip()
        if s.startswith("#"):
            continue
        if i + 1 < len(lines) and lines[i + 1].strip() == "{" and s.endswith(")"):
            m = re.search(r"([A-Za-z_][A-Za-z0-9_]*)\s*\(", s)
            if m and not s.startswith("if") and not s.startswith("while") and not s.startswith("for"):
                return m.group(1)
    return None


def defined_functions(lines):
    """按 `...name(...)` + 下一行 '{' 的启发式收集本文件定义的函数名（用于 A10b）"""
    names = []
    for i, line in enumerate(lines):
        s = line.strip()
        if s.endswith(")") and i + 1 < len(lines) and lines[i + 1].strip() == "{":
            m = re.search(r"([A-Za-z_][A-Za-z0-9_]*)\s*\(", s)
            if m and not s.startswith("if") and not s.startswith("while") and not s.startswith("for"):
                names.append(m.group(1))
    return names


def find_calls(sources, pattern, skip_defs=True):
    """[(文件, 行号(1-based), 所在函数)]
    skip_defs=True 时跳过**定义行**（该行以 ')' 结尾、下一行是 '{'）—— 否则
    `static esp_err_t wifi_prov_connect(...)` 这类定义会被当成一次"调用"。"""
    hits = []
    for fn, lines in sources.items():
        for i, line in enumerate(lines):
            s = line.lstrip()
            if s.startswith("*") or s.startswith("/*") or s.startswith("//"):
                continue
            if skip_defs and i + 1 < len(lines) and lines[i + 1].strip() == "{":
                continue
            if pattern.search(line):
                hits.append((fn, i + 1, enclosing_function(lines, i)))
    return hits


def run_checks(sources, headers, test_lines):
    """返回 (ok, failures[])；headers = {相对路径: 行列表}"""
    fails = []

    def expect(hits, n, what, detail):
        if len(hits) != n:
            fails.append("{}: 期望 {} 处，实测 {} 处 —— {}".format(what, n, len(hits), detail))
        return hits

    def need(cond, what, detail):
        if not cond:
            fails.append("{} —— {}".format(what, detail))

    policy_h = headers.get("wifi_prov_policy.h", [])
    prov_h = headers.get("wifi_prov.h", [])
    policy_text = "\n".join(policy_h)
    prov_h_text = "\n".join(prov_h)
    prov_c = sources.get("wifi_prov.c", [])
    prov_c_text = "\n".join(prov_c)
    main_lines = sources.get("korvo2_oneye_main.c", [])

    # ---------------------------------------------------------------- A1–A7（收敛点唯一）
    # A1 `oneye_start()` 只有 1 处调用点，且在 cloud_start_task 内
    h = expect(find_calls(sources, CALL_ONEYE_START), 1,
               "A1 oneye_start() 调用点", "任何新增调用点都绕过/复制了收敛点")
    if h and h[0][2] != "cloud_start_task":
        fails.append("A1 oneye_start() 的调用点不在 cloud_start_task 内（实测在 {}）".format(h[0][2]))

    # A2 cloud_start_task 只有 1 处创建点，且在 on_wifi_ready 内
    h = expect(find_calls(sources, CREATE_CLOUD_TASK), 1,
               "A2 cloud_start_task 创建点", "多于 1 处 = 有第二条通往 oneye_start() 的路径")
    if h and h[0][2] != "on_wifi_ready":
        fails.append("A2 cloud_start_task 的创建点不在 on_wifi_ready 内（实测在 {}）".format(h[0][2]))

    # A3 on_wifi_ready 只有 1 处注册点，且在 app_main（IDF 入口）内
    h = expect(find_calls(sources, REG_READY_CB), 1,
               "A3 on_wifi_ready 的 ready 回调注册点", "通道不应各自注册自己的 ready 回调")
    if h and h[0][2] != "app_main":
        fails.append("A3 on_wifi_ready 的注册点不在 app_main 内（实测在 {}）".format(h[0][2]))

    # A4 wifi_prov.c 里 ready 回调只有 1 处触发点，且在 on_wifi_event 的 GOT_IP 分支里
    h = expect(find_calls({"wifi_prov.c": prov_c}, CALL_READY_CB), 1,
               "A4 s_ready_cb() 触发点（wifi_prov.c）", "唯一触发点必须只有一处")
    if h:
        fn, ln, func = h[0]
        if func != "on_wifi_event":
            fails.append("A4 s_ready_cb() 的触发点不在 on_wifi_event 内（实测在 {}）".format(func))
        window = "\n".join(prov_c[max(0, ln - 25):ln])
        if not GOT_IP.search(window):
            fails.append("A4 s_ready_cb() 的触发点不在 IP_EVENT_STA_GOT_IP 分支的近邻（上溯 25 行内未见该事件）")

    # A5 通道 C1/C2（BLE）：只经唯一入口交凭据，且自己不调 oneye_start()
    if "wifi_prov_ble.c" not in sources:
        fails.append("A5 缺少通道模块 wifi_prov_ble.c（BLE 通道 C1/C2）")
    else:
        b = sources["wifi_prov_ble.c"]
        expect(find_calls({"wifi_prov_ble.c": b}, SUBMIT_CREDS), 1,
               "A5 wifi_prov_ble.c 调用 wifi_prov_submit_credentials 的次数",
               "BLE 通道必须且只能经唯一入网入口交凭据")
        if not [i + 1 for i, l in enumerate(b) if CHAN_BLE.search(l)]:
            fails.append("A5 wifi_prov_ble.c 未使用 WIFI_PROV_CHAN_BLE（优先级判据拿不到通道身份）")
        if find_calls({"wifi_prov_ble.c": b}, CALL_ONEYE_START):
            fails.append("A5 wifi_prov_ble.c 里出现了 oneye_start() 调用（通道不得自己触发上云）")
        if any(re.search(r"esp_wifi_connect\s*\(", l) for l in b):
            fails.append("A5 wifi_prov_ble.c 自己调了 esp_wifi_connect()（Wi-Fi 生命周期归 wifi_prov.c）")

    # A6 通道 C3（凭据文件）+ C5（Kconfig）都经**唯一入网入口**进入同一下游
    if not main_lines:
        fails.append("A6 缺少 korvo2_oneye_main.c")
    else:
        if not any(LOAD_FILE_CALL.search(l) for l in main_lines):
            fails.append("A6 korvo2_oneye_main.c 未调用 wifi_prov_load_file（C3 通道缺失）")
        h = expect(find_calls({"korvo2_oneye_main.c": main_lines}, SUBMIT_CREDS_SRC), 2,
                   "A6 korvo2_oneye_main.c 经唯一入口提交凭据的次数（C3 文件 + C5 Kconfig）",
                   "C3/C5 必须经 wifi_prov_submit_credentials_src() 进入判据，不得直连连接层")

    # A7 除 main.c 外，任何源文件都不得出现 oneye_start 调用
    for fn, lines in sources.items():
        if fn == "korvo2_oneye_main.c":
            continue
        if find_calls({fn: lines}, CALL_ONEYE_START):
            fails.append("A7 {} 里出现了 oneye_start() 调用（收敛点被复制）".format(fn))

    # ---------------------------------------------------------------- A8 优先级可判定
    # A8a 启动期先递 SD（C3）、后递 BLE（C1/C2）：各 1 处，且行序正确；失败后必须显式释放采信槽
    lf = expect(find_calls({"korvo2_oneye_main.c": main_lines}, LOAD_FILE_CALL), 1,
                "A8a wifi_prov_load_file() 调用点", "C3（SD 卡凭据）必须在 wifi_prov_boot() 里被递上去")
    bs = expect(find_calls({"korvo2_oneye_main.c": main_lines}, BLE_START_CALL), 1,
                "A8a wifi_prov_ble_start() 调用点", "C1/C2 必须在 wifi_prov_boot() 里被递上去")
    if lf and lf[0][2] != "wifi_prov_boot":
        fails.append("A8a wifi_prov_load_file() 不在 wifi_prov_boot() 内（实测在 {}）".format(lf[0][2]))
    if bs and bs[0][2] != "wifi_prov_boot":
        fails.append("A8a wifi_prov_ble_start() 不在 wifi_prov_boot() 内（实测在 {}）".format(bs[0][2]))
    if lf and bs and not (lf[0][1] < bs[0][1]):
        fails.append("A8a 优先级顺序错：SD 卡凭据（第 {} 行）必须**先于** BLE 通道（第 {} 行）被递上去"
                     "（口径 SD > BLE > 其他）".format(lf[0][1], bs[0][1]))
    expect(find_calls({"korvo2_oneye_main.c": main_lines}, RELEASE_HELD_CALL), 1,
           "A8a SD 凭据入网失败后的 wifi_prov_release_held() 降级点",
           "不释放采信槽，低优先级通道会被判据永久挡住（配网死锁）")

    # A8b 连接层不可旁路：除 wifi_prov.c 外任何 .c 都不得调 wifi_prov_connect()
    for fn, lines in sources.items():
        if fn == "wifi_prov.c":
            continue
        if find_calls({fn: lines}, CONNECT_CALL):
            fails.append("A8b {} 直接调了 wifi_prov_connect()（绕过优先级判据）".format(fn))
    expect(find_calls({"wifi_prov.c": prov_c}, CONNECT_CALL), 1,
           "A8b wifi_prov.c 内 wifi_prov_connect() 的调用点",
           "连接层只应被 wifi_prov_submit_credentials_src() 调用一次")

    # A8c 连接层必须是模块内部 static，且不出现在头文件里
    need(re.search(r"^\s*static\s+esp_err_t\s+wifi_prov_connect\s*\(", prov_c_text, re.M) is not None,
         "A8c wifi_prov.c 的 wifi_prov_connect() 不是 static",
         "连接层一旦对外可见，判据就能被旁路")
    need(re.search(r"^\s*(?:esp_err_t|void|int)\s+wifi_prov_connect\s*\(", prov_h_text, re.M) is None,
         "A8c wifi_prov.h 仍在声明 wifi_prov_connect()",
         "连接层不得进入公开头文件（模块外拿不到 ⇒ 无法绕过判据）")

    # A8d 通道全部经唯一入网入口；来源标签不得被通道侧旁路改写
    need(len(find_calls({"media_api.c": sources.get("media_api.c", [])}, SUBMIT_CREDS)) >= 1,
         "A8d 台面 API 通道（C4）未走唯一入网入口 wifi_prov_submit_credentials()",
         "C4 也必须过判据（显式改配走 release_held + submit）")
    for fn, lines in sources.items():
        if fn == "wifi_prov.c":
            continue
        if find_calls({fn: lines}, SET_SOURCE_CALL):
            fails.append("A8d {} 直接调了 wifi_prov_set_source()（来源标签应只在唯一入口里写）".format(fn))

    # A8e 五种决策都有可观测落点（否则判据是"判了但不留痕"）
    for name in DECISION_ENUM:
        need(name in policy_text, "A8e 判据缺少决策枚举 {}".format(name),
             "五种裁决（首采/同通道/抢占/低优先级拒绝/已入网拒绝）必须齐全")
    for token, what in (("wifi_prov_policy_decide(", "调用唯一判据"),
                        ("wifi_prov_decision_str(", "把决策 token 打进日志"),
                        ("[prov-priority]", "留痕前缀"),
                        ("ESP_ERR_INVALID_STATE", "拒绝时返回可判定的错误码")):
        need(token in prov_c_text, "A8e wifi_prov.c 缺少 {}".format(what),
             "期望出现 token：{}".format(token))

    # A8f 判据单源：公开头文件必须 include 判据头（而不是各写一份）
    need(re.search(r'#include\s+"wifi_prov_policy\.h"', prov_h_text) is not None,
         "A8f wifi_prov.h 未 include wifi_prov_policy.h",
         "通道枚举与优先级判据必须单源（否则固件与单测会各跑一份）")

    # ---------------------------------------------------------------- A9 只入网一次
    if main_lines:
        guard = [i + 1 for i, l in enumerate(main_lines) if re.search(r"static\s+bool\s+started\s*;", l)]
        need(len(guard) == 1, "A9a on_wifi_ready() 的一次性守卫 `static bool started;` 不唯一",
             "实测 {} 处".format(len(guard)))
        if len(guard) == 1:
            fn = enclosing_function(main_lines, guard[0] - 1)
            need(fn == "on_wifi_ready", "A9a 一次性守卫不在 on_wifi_ready() 内（实测在 {}）".format(fn),
                 "守卫必须与上云任务创建在同一函数内")
            cr = find_calls({"korvo2_oneye_main.c": main_lines}, CREATE_CLOUD_TASK)
            if cr and not (cr[0][1] > guard[0]):
                fails.append("A9a cloud_start_task 的创建点（第 {} 行）在一次性守卫（第 {} 行）之前"
                             "⇒ 守卫拦不住第二次入网".format(cr[0][1], guard[0]))

    need(re.search(r"wifi_prov_policy_decide\(\s*s_held_valid\s*,\s*s_held_chan\s*,\s*ch\s*,"
                   r"\s*s_connected\s*&&\s*s_held_valid\s*\)", prov_c_text) is not None,
         "A9b wifi_prov.c 传给判据的 onboarded 不是 `s_connected && s_held_valid`",
         "「只入网一次」必须由「已入网且采信槽未被显式释放」决定")

    i_onboarded = policy_text.find("if (onboarded)")
    i_held = policy_text.find("if (!held_valid)")
    need(i_onboarded >= 0, "A9c 判据里没有 `if (onboarded)` 分支（只入网一次缺失）", "见 wifi_prov_policy.h")
    need(i_held >= 0, "A9c 判据里没有 `if (!held_valid)` 分支（首次有效缺失）", "见 wifi_prov_policy.h")
    if i_onboarded >= 0 and i_held >= 0:
        need(i_onboarded < i_held, "A9c 判据顺序错：`onboarded` 必须在 `!held_valid` 之前",
             "否则已入网设备的空槽会被再次采信（重复入网）")

    clears = prov_c_text.count("s_held_valid = false")
    need(clears >= 2, "A9d 清采信槽的路径不足（实测 {} 处）".format(clears),
         "至少 `wifi_prov_release_held()` 与 `wifi_prov_forget()` 两处都要清")

    # ---------------------------------------------------------------- A10 夹具同源 + 非空转
    if not test_lines:
        fails.append("A10a 缺少主机侧夹具 tools/test-prov-priority.c")
    else:
        t = "\n".join(test_lines)
        need(re.search(r'#include\s+"wifi_prov_policy\.h"', t) is not None,
             "A10a 夹具未 include 固件的 wifi_prov_policy.h（判据抄件）",
             "夹具必须编译固件里跑的那一份判据")
        dup = [n for n in defined_functions(test_lines) if n in JUDGE_NAMES]
        need(not dup, "A10b 夹具自定义了判据函数（抄件）：{}".format(",".join(sorted(set(dup)))),
             "夹具只能引用固件判据，不得自备一份")
        for tag in ("[夹具①]", "[夹具②]", "[夹具③]"):
            need(tag in t, "A10c 夹具缺少 {}".format(tag),
                 "三个夹具：SD 胜出 / BLE 胜出 / 重复入网被拒")
        need("mutant_decide" in t and "负向对照" in t,
             "A10d 夹具缺少变异负向对照（mutant_decide / 负向对照）",
             "没有负向对照 ⇒ 夹具可能空转")

    # ------------------------------------------- A11 自检/授时只能从收敛点下游调用（所有通道共用）
    # 形状（2026-10-07 起）：on_wifi_ready() 内 入网日志 → net_probe → sntp → 面板 → cloud_start_task。
    # 旧结构是 `prov_boot_after_connect()`（只被 C3/C5 成功路径调用）⇒ BLE 通道会跳过这两步：
    # net_probe 只丢诊断；sntp 则是**真风险**（严格 TLS 需要有效时钟 ⇒ 可能连不上 ⇒ bootstrap 死锁）。
    np_hits = expect(find_calls(sources, CALL_NET_PROBE), 1,
                     "A11a net_probe_report() 调用点",
                     "多于 1 处 = 某条通道又自建了一遍入网后自检（第二条路径）")
    sb_hits = expect(find_calls(sources, CALL_SNTP_BOOT), 1,
                     "A11b sntp_boot_sync() 调用点",
                     "多于 1 处 = 某条通道又自建了一遍入网后授时（第二条路径）")
    for hits, what in ((np_hits, "net_probe_report()"), (sb_hits, "sntp_boot_sync()")):
        for fn, ln, func in hits:
            if fn != "korvo2_oneye_main.c" or func != "on_wifi_ready":
                fails.append("A11c {} 的调用点在 {}:{}（所在函数 {}）—— 只允许在 "
                             "korvo2_oneye_main.c 的 on_wifi_ready() 内（收敛点下游）"
                             .format(what, fn, ln, func))
    for ch in CHANNEL_MODULES:
        lines = sources.get(ch)
        if not lines:
            continue
        for pat, what in ((CALL_NET_PROBE, "net_probe_report()"),
                          (CALL_SNTP_BOOT, "sntp_boot_sync()")):
            if find_calls({ch: lines}, pat):
                fails.append("A11d 通道模块 {} 里出现了 {} —— 通道只负责交凭据，"
                             "入网后动作归收敛点下游".format(ch, what))

    # ------------------------------------------- A12 顺序：自检 → 授时 → 面板 → cloud_start_task
    if not np_hits or not sb_hits:
        fails.append("A12 缺少可判定顺序的调用点（net_probe_report / sntp_boot_sync 不在场）")
    else:
        np_ln, sb_ln = np_hits[0][1], sb_hits[0][1]
        if not (np_hits[0][2] == "on_wifi_ready" and sb_hits[0][2] == "on_wifi_ready"):
            fails.append("A12 自检/授时的调用点不在 on_wifi_ready() 内 ⇒ 顺序无法与 cloud_start_task 比较")
        create = find_calls({"korvo2_oneye_main.c": main_lines}, CREATE_CLOUD_TASK)
        if not create:
            fails.append("A12 找不到 xTaskCreate(cloud_start_task, …) 创建点 ⇒ 顺序不可判定")
        else:
            cr_ln = create[0][1]
            if not (np_ln < sb_ln):
                fails.append("A12a 顺序错：net_probe_report()（第 {} 行）必须在 sntp_boot_sync()"
                             "（第 {} 行）之前（日志顺序即「联网 → 自检 → 授时 → 上云」）"
                             .format(np_ln, sb_ln))
            if not (sb_ln < cr_ln):
                fails.append("A12b 顺序错：sntp_boot_sync()（第 {} 行）必须在 cloud_start_task 创建点"
                             "（第 {} 行）**之前** —— 严格 TLS 校验需要先把时钟校好"
                             .format(sb_ln, cr_ln))
            pn = [h for h in find_calls({"korvo2_oneye_main.c": main_lines}, CALL_PANEL_START)
                  if h[2] == "on_wifi_ready"]
            need(len(pn) == 1, "A12c on_wifi_ready() 内 panel_start_if_enabled() 调用点",
                 "期望恰好 1 处，实测 {} 处（面板与云链路解耦，但仍在收敛点下游且只调一次）"
                 .format(len(pn)))
            if len(pn) == 1 and not (sb_ln < pn[0][1] < cr_ln):
                fails.append("A12c 顺序错：面板调用（第 {} 行）必须落在 sntp（第 {} 行）与 "
                             "cloud_start_task 创建点（第 {} 行）之间"
                             .format(pn[0][1], sb_ln, cr_ln))

    return (not fails), fails


def run_host_test(root):
    """实跑主机侧夹具（cc/gcc 可用时）。返回 (status, detail)；status ∈ {pass, fail, skip}"""
    test = os.path.join(root, TEST_REL)
    cc = shutil.which("cc") or shutil.which("gcc")
    if cc is None:
        return "skip", "PATH 上找不到 cc/gcc ⇒ 夹具未实跑（A8/A9 的静态断言仍然生效）"
    tmp = tempfile.mkdtemp(prefix="prov-prio-")
    try:
        exe = os.path.join(tmp, "tpp")
        cmd = [cc, "-std=c99", "-Wall", "-Wextra", "-Werror",
               "-I", os.path.join(root, MAIN_REL), test, "-o", exe]
        comp = subprocess.run(cmd, capture_output=True, text=True)
        if comp.returncode != 0:
            return "fail", "夹具编译失败（exit {}）：{}".format(comp.returncode, comp.stderr.strip()[:400])
        run = subprocess.run([exe], capture_output=True, text=True)
        out = (run.stdout or "") + (run.stderr or "")
        tail = [l for l in out.splitlines() if l.startswith("RESULT=") or "负向对照" in l]
        if run.returncode != 0:
            return "fail", "夹具运行失败（exit {}）：{}".format(run.returncode, " | ".join(tail))
        if "RESULT=PASS" not in out:
            return "fail", "夹具未给出 RESULT=PASS：{}".format(" | ".join(tail))
        if "负向对照通过" not in out:
            return "fail", "夹具未证明负向对照报红（空转）：{}".format(" | ".join(tail))
        return "pass", " | ".join(tail)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


# ------------------------------------------------------------------ 负向对照注入

def _replace_in(lines, old, new, expect_count=None):
    """替换注入。expect_count=None ⇒ 至少命中 1 处（不硬编码处数，免得注释改动就失效）。"""
    hits = 0
    out = []
    for l in lines:
        if old in l:
            hits += l.count(old)
            l = l.replace(old, new)
        out.append(l)
    if expect_count is None:
        if hits < 1:
            raise RuntimeError("注入失败：'{}' 一处都没命中".format(old))
    elif hits != expect_count:
        raise RuntimeError("注入失败：期望命中 {} 处 '{}'，实测 {} 处".format(expect_count, old, hits))
    return out


def _append(lines, payload):
    return list(lines) + payload.splitlines()


def _move_sntp_after_cloud_start(lines):
    """注入：把 `(void)sntp_boot_sync();` 从原位删掉，改插到 on_wifi_ready() 末尾
    （= `xTaskCreate(cloud_start_task,…)` **之后**）。调用点总数仍为 1，只有**顺序**变坏 ⇒
    只有 A12b（以及 A12c 的面板相对顺序）能抓住它 —— 这正是"顺序"断言不是空转的证据。"""
    call = "(void)sntp_boot_sync();"
    idx = [i for i, l in enumerate(lines) if call in l]
    if len(idx) != 1:
        raise RuntimeError("注入失败：期望命中 1 处 '{}'，实测 {} 处".format(call, len(idx)))
    out = [l for i, l in enumerate(lines) if i != idx[0]]
    anchor = [i for i, l in enumerate(out) if "已联网 → 启动上云" in l]
    if len(anchor) != 1:
        raise RuntimeError("注入失败：找不到 on_wifi_ready() 末尾的启动日志行（锚点）")
    out.insert(anchor[0] + 1, "    " + call)
    return out


INJECTIONS = [
    # (说明, 文件相对 main/ 的路径, 目标断言 id, 变异函数)
    ("第二条入网后逻辑（在 BLE 通道里直接调 oneye_start()）", "wifi_prov_ble.c", "A1",
     lambda ls: _append(ls, "\nstatic void injected_second_convergence(void) { oneye_start(); }")),
    ("BLE 通道直连连接层（绕过优先级判据）", "wifi_prov_ble.c", "A8b",
     lambda ls: _append(ls, "\nstatic int injected_bypass(const char *s, const char *p) "
                            "{ return wifi_prov_connect(s, p, 0); }")),
    ("判据实参丢掉采信槽状态（只入网一次失效）", "wifi_prov.c", "A9b",
     lambda ls: _replace_in(ls, "s_connected && s_held_valid", "s_connected")),
    ("决策留痕被抹掉（判了但不留痕）", "wifi_prov.c", "A8e",
     lambda ls: _replace_in(ls, "[prov-priority]", "prov")),
    # A11/A12（2026-10-07 新增）：入网后的自检/授时只能从收敛点下游调用，且必须在建链之前。
    ("通道模块里再调一次 sntp_boot_sync()（BLE 通道自建入网后逻辑）", "wifi_prov_ble.c", "A11",
     lambda ls: _append(ls, "\nstatic void injected_channel_sntp(void) { (void)sntp_boot_sync(); }")),
    ("通道模块里再调一次 net_probe_report()（台面 API 自建入网后逻辑）", "media_api.c", "A11",
     lambda ls: _append(ls, "\nstatic void injected_channel_probe(void) "
                            "{ net_probe_report(\"host\", 18886); }")),
    ("把 sntp_boot_sync() 挪到 cloud_start_task 之后（顺序退化）", "korvo2_oneye_main.c", "A12",
     _move_sntp_after_cloud_start),
]


def build_mutant(root, tmp_root):
    """把 main/ 与夹具整棵树复制到 tmp_root，逐条注入，返回 [(说明, 目标断言, 变异根)]"""
    shutil.copytree(os.path.join(root, MAIN_REL), os.path.join(tmp_root, MAIN_REL))
    os.makedirs(os.path.join(tmp_root, TOOLS_REL), exist_ok=True)
    shutil.copyfile(os.path.join(root, TEST_REL), os.path.join(tmp_root, TEST_REL))

    mutants = []
    for desc, rel, target, mutate in INJECTIONS:
        mroot = tempfile.mkdtemp(prefix="prov-conv-neg-", dir=tmp_root)
        shutil.copytree(os.path.join(tmp_root, MAIN_REL), os.path.join(mroot, MAIN_REL))
        os.makedirs(os.path.join(mroot, TOOLS_REL), exist_ok=True)
        shutil.copyfile(os.path.join(tmp_root, TEST_REL), os.path.join(mroot, TEST_REL))
        path = os.path.join(mroot, MAIN_REL, rel)
        lines = read_lines(path)
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(mutate(lines)) + "\n")
        mutants.append((desc, target, mroot))
    return mutants


def mutated_test_copy(root, tmp_root):
    """另一类注入：把**夹具自己**抄一份判据（A10b 必须报红）"""
    mroot = tempfile.mkdtemp(prefix="prov-conv-neg-t-", dir=tmp_root)
    shutil.copytree(os.path.join(root, MAIN_REL), os.path.join(mroot, MAIN_REL))
    os.makedirs(os.path.join(mroot, TOOLS_REL), exist_ok=True)
    lines = read_lines(os.path.join(root, TEST_REL))
    lines = _append(lines, "\nstatic inline wifi_prov_decision_t "
                           "wifi_prov_policy_decide(bool a, wifi_prov_channel_t b, "
                           "wifi_prov_channel_t c, bool d)\n"
                           "{\n    (void)a; (void)b; (void)c; (void)d; return 0;\n}")
    with open(os.path.join(mroot, TEST_REL), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    return ("夹具自备一份判据（抄件）", "A10b", mroot)


def load(root):
    sources = read_sources(root)
    headers = {}
    for name in ("wifi_prov.h", "wifi_prov_policy.h"):
        p = os.path.join(root, MAIN_REL, name)
        headers[name] = read_lines(p) if os.path.isfile(p) else []
    test = os.path.join(root, TEST_REL)
    test_lines = read_lines(test) if os.path.isfile(test) else []
    return sources, headers, test_lines


def main():
    if not os.path.isdir(os.path.join(EXAMPLE, MAIN_REL)):
        print("FAIL: 找不到 main/ 目录: {}".format(os.path.join(EXAMPLE, MAIN_REL)))
        return 1

    src, headers, test_lines = load(EXAMPLE)
    print("扫描 {} 个 .c 文件 + 2 个头文件 + 1 个夹具（{}）".format(len(src), EXAMPLE))
    ok, fails = run_checks(src, headers, test_lines)
    for f in fails:
        print("  ✗ " + f)
    print("正向断言（A1–A12）：{}".format("PASS" if ok else "FAIL"))

    status, detail = run_host_test(EXAMPLE)
    if status == "pass":
        print("夹具实跑：PASS（{}）".format(detail))
    elif status == "fail":
        print("夹具实跑：FAIL（{}）".format(detail))
        fails.append("A10e 主机侧夹具实跑未通过：{}".format(detail))
        ok = False
    else:
        print("夹具实跑：SKIP（{}）".format(detail))

    # ---- 负向对照：逐条注入已知缺陷，对应断言必须报红 ----
    tmp_root = tempfile.mkdtemp(prefix="prov-conv-neg-all-")
    ng_ok = True
    try:
        mutants = build_mutant(EXAMPLE, tmp_root) + [mutated_test_copy(EXAMPLE, tmp_root)]
        for desc, target, mroot in mutants:
            msrc, mheaders, mtest = load(mroot)
            m_ok, m_fails = run_checks(msrc, mheaders, mtest)
            caught = any(f.startswith(target) for f in m_fails)
            if m_ok or not caught:
                ng_ok = False
                print("负向对照：FAIL（注入「{}」后仍判为通过/未被 {} 抓住）".format(desc, target))
                for f in m_fails:
                    print("    · " + f)
            else:
                hit = next(f for f in m_fails if f.startswith(target))
                print("负向对照：PASS（注入「{}」被 {} 捕获：{}）".format(desc, target, hit[:120]))
    finally:
        shutil.rmtree(tmp_root, ignore_errors=True)

    if not ng_ok:
        return 1
    if not ok:
        return 1
    print("RESULT=PASS 单一收敛点 + 优先级（SD>BLE>其他）+ 只入网一次 + 自检/授时全通道必经"
          "（ADR-0017 D2/D4）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
