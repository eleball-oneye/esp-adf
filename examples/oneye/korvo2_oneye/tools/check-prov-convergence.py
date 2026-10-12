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
         ⚠️ 2026-10-12 起：「已入网一律拒绝」**只对非权威通道**成立（权威通道 = BLE/App，见 A15）
  A10    夹具与判据**同源**（不抄件）且**自带变异负向对照**，并实跑一次（有 cc 时）
  A11    入网后的**启动期自检与授时**（net_probe_report / sntp_boot_sync）各只有 1 处调用点，
         且都在 korvo2_oneye_main.c 的 on_wifi_ready() 内（= 收敛点下游）⇒ **所有通道**都经过它们；
         通道模块（wifi_prov_ble.c / media_api.c / wifi_prov.c / panel_api.c …）里不得出现这两个符号
  A12    收敛点下游的**顺序**：net_probe → sntp → 面板 → `xTaskCreate(cloud_start_task,…)`。
         ⚠️ sntp 必须在 cloud_start_task 之前：严格 TLS 校验需要先把时钟校好（设备无 RTC）
  A13    资源读数（`[res]`：free_internal / largest_internal_block / free_psram）必须是**同一个** helper
         （全工程只有 1 处日志格式串）在 3 个**未入网也必经**的点各打一行、且带 `phase=` 阶段标签：
         `app_main()`（配网/NimBLE 之前）· `wifi_prov_boot()` 内 `wifi_prov_ble_start()` **之后**
         （NimBLE 已起栈、尚未入网）· `cloud_start_task()`（原有的入网后那一处，⛔ 不得被删/移）。
         旧形状只有第 3 处 ⇒ BLE 机型未入网时这一行**永远打不出来**（2026-10-07 真机实证：
         BLE 件 120 s 抓包 `[res]` 命中 0 行），"NimBLE 12 KB 主机栈之后内部 RAM 还剩多少"因此无读数。
  A14    🔴 2026-10-10（本批）：**落实既有契约状态 `prov.status{state=connected, ip=…}`** ——
         `connected` 是 `contracts/local/link-frames.json` 的 `prov_states` 里**既有**项，
         契约同处写明「connected 时 ip 必填」；本批只是把它**发出去**，⛔ 不新增帧 / ⛔ 不新增错误码。
         A14a `oneye_dev_link_prov_report_status()` 在 main/ 下恰好 1 处调用，且在
              `prov_status_report_connected_once()` 内（⛔ 不许有第二个上报点）
         A14b 这一处传的状态**必须**是 `ONEYE_DEV_LINK_PROV_CONNECTED`（⛔ 不许改报别的状态）
         A14c 该 helper 的调用点恰好 1 处，且在收敛点下游 `on_wifi_ready()` 内
         A14d 判据**单源**：本体只在 `wifi_prov_status.h`（固件与夹具编译同一份），
              main/ 下的 .c ⛔ 不得自备一份（抄件）
         A14e 判据里**有**"ip 非空"这一条（⛔ 拿不到 IP 不许报 connected —— App 判据是
              `state=="connected" && ip 非空`）
         A14f 判据里**有**"只报一次"这一条（⛔ 不刷屏：收敛点每次 GOT_IP 都会被调）
         A14g 幂等位**只在发送成功后**置位（失败 ⇒ 下一次 GOT_IP 仍补报，⛔ 不把发失败写成已上报）
         A14h ⛔ 不新增帧 / 不新增状态：main/ 下 `ONEYE_DEV_LINK_PROV_CONNECTED` 恰好 1 处、
              且**不得**出现 `prov.status` 帧名字面量（帧名由 SDK 组，设备侧不自造帧）
  A15    🔴 2026-10-12（本批 · 用户裁定「**已有app发起配网时设备要断现有wifi重新连接目标wifi**，
         否则app会卡在配网中这个流程。」）：**权威通道（BLE/App）"断旧连新"** ——
         A15a 判据**按通道分派**：`wifi_prov_policy.h` 有 `wifi_prov_channel_is_authoritative()`，
              且**只**对 `WIFI_PROV_CHAN_BLE` 为真（⛔ 不是把别的通道一起放进来）
         A15b 「已入网一律拒绝」被权威通道排除 ⇒ `REJECT_ALREADY_UP` 在 **BLE 通路不可达**
              （若改回 `if (onboarded)` ⇒ 本组必须报红）
         A15c `wifi_prov.c` 的 `wifi_prov_submit_credentials_src()` 内**恰好一次**
              `esp_wifi_disconnect()`，且受"在线才断"约束（`if (s_connected)` 段内），
              且在 `wifi_prov_connect(` 调用**之前**（"断旧"在"连新"之前）
         A15d 断链**之前**先置抑制位 `s_reprovision`，且 `on_wifi_event()` 的 DISCONNECTED 分支
              认这个位 —— 否则断链事件会用**旧配置**自动重连（"换网"退化成"重连旧网"）
         A15e 同一段里旧采信态作废（`s_held_valid = false`）且新通道随后落槽（`s_held_chan = ch`）
         A15f ⛔ 通道模块（`wifi_prov_ble.c`）自己**不得**碰 `esp_wifi_disconnect()`
              （Wi-Fi 生命周期归 `wifi_prov.c`；通道只交凭据）
         ⛔ 本组**不新增帧 / 不新增状态 / 不动 Kconfig 与机读契约**：它只断言既有枚举与既有 API 的用法。
  A16    🔴 2026-10-12（本批 · 真机证据）：**`prov.status{connected}` 幂等位的「会话边界复位」**——
         15:25 第一轮报过一次 connected；15:36 第二轮（App 走 `[prov-switch]` 断旧连新）拿到 IP 之后
         **再也没有报** ⇒ App 一直停在"配网中"到超时。成因 = 幂等位**跨会话不复位**。
         A16a 复位跃迁本体在 `wifi_prov_status.h`（`wifi_prov_connected_reported_after_disconnect()`），
              且**真复位**（`return false`；⛔ `return already_reported` = 永不复位 = 旧行为）
         A16b 复位边锚在 `wifi_prov.c` 的 `WIFI_EVENT_STA_DISCONNECTED` 分支（**会话边界**）：
              会话结束通告 `s_disconnected_cb()` 恰好 1 处、且落在该分支内（未越过 GOT_IP 分支）
         A16c 复位**只**由会话结束边推进：`wifi_prov_connected_reported_after_disconnect()` 在 main/ 下
              恰好 1 处调用，且⛔ **不在** `prov_status_report_connected_once()` 内
              （塞进上报函数 = 每次 GOT_IP 都复位 ⇒ 退化成"每次都报"）
         A16d 会话结束回调注册点唯一，且在 `app_main()` 内（与 ready 回调同一处生命周期注册点）
         ⛔ 本组**不放宽判据**：① ip 非空 / ② 只报一次 仍由 A14e/A14f 断言；复位的是**状态**。
         ⛔ 不新增帧 / 不新增状态 / 不新增错误码：`prov.status{connected}` 的 payload 与取值沿用。

退出码：0 = 全部断言通过；1 = 有断言失败（逐条打出）。
**自带负向对照**：把 `main/` 与夹具复制到临时目录后逐条注入已知缺陷，同一套检查**必须逐条报红**
（含 A1 的第二条入网后逻辑、A8b 的直连连接层、A9b 的丢掉"只入网一次"、A8e 的留痕缺失、
A10b 的判据抄件、**A11 的"通道模块里再调一次 sntp_boot_sync()/net_probe_report()"**、
**A12 的"把 sntp 挪到 cloud_start_task 之后"**、**A13 的"删掉 NimBLE 起栈后那一处资源读数"**）。
**A14 的「丢掉 ip 非空判据 / 丢掉幂等判据 / 发送失败也置位 / 第二个上报点 / 改报别的状态」**。
**A15 的「把 REJECT_ALREADY_UP 放回 / 删掉断旧前置段 / 抹掉抑制位」。**
**A16 的「断开边不复位（旧行为）/ 复位跃迁改成恒等 / 把复位挪进上报函数（每次都报）」。**
任一条注入没有被对应断言抓住 ⇒ 本脚本自己的判据是空转的 ⇒ exit 1。
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
# A13：资源读数（`[res]`）必须在**不依赖入网**的必经点各打一行、且带阶段标签（2026-10-07 第三批）
CALL_RES_REPORT = re.compile(r"res_report\s*\(")
RES_LOG_FORMAT = re.compile(r'ESP_LOGI\s*\(\s*TAG\s*,\s*"\[res\]\s+phase=%s')
RES_RESERVED_KINDS = ("app_main", "wifi_prov_boot", "cloud_start_task")
# A14：`prov.status{state=connected, ip=…}` 上报（2026-10-10 本批：落实**既有**契约状态）
CALL_PROV_REPORT = re.compile(r"oneye_dev_link_prov_report_status\s*\(")
CALL_PROV_WRAPPER = re.compile(r"wifi_prov_ble_report_prov_connected\s*\(")
CALL_PROV_CONNECTED_REPORT = re.compile(r"prov_status_report_connected_once\s*\(")
CALL_STATUS_JUDGE = re.compile(r"wifi_prov_connected_should_report\s*\(")
CALL_STATUS_MARK = re.compile(r"wifi_prov_connected_reported_after\s*\(")
PROV_CONNECTED_TOKEN = re.compile(r"ONEYE_DEV_LINK_PROV_CONNECTED")
FRAME_NAME_LITERAL = re.compile(r'"(prov\.status)"')
STATUS_HEADER = "wifi_prov_status.h"
STATUS_HELPER = "prov_status_report_connected_once"
STATUS_WRAPPER = "wifi_prov_ble_report_prov_connected"
STATUS_WRAPPER_FILE = "wifi_prov_ble.c"
STATUS_JUDGE_DEF = re.compile(r"bool\s+wifi_prov_connected_should_report\s*\(")
STATUS_IP_GUARD = re.compile(r"ip\[0\]\s*==\s*'\\0'")
STATUS_ONCE_GUARD = re.compile(r"if\s*\(\s*already_reported\s*\)")
# 通道模块（判据侧只允许"交凭据"；出现上面两个符号即第二条入网后路径）
CHANNEL_MODULES = ("wifi_prov_ble.c", "media_api.c", "wifi_prov.c", "panel_api.c",
                   "wifi_prov_file.c", "wifi_prov_sonic.c", "wifi_prov_qrcode.c")
# A15（2026-10-12 本批）：权威通道（BLE/App）"断旧连新"
#   ⚠️ 用 strip_comments 之后的**代码面**判定（注释里的旧字留痕不得被当实现或漏判）
AUTH_CHAN_HELPER = "wifi_prov_channel_is_authoritative"
AUTH_GUARD = "if (onboarded && !authoritative)"
DISCONNECT_CALL = "esp_wifi_disconnect()"
REPROVISION_FLAG = "s_reprovision"
# A16（🔴 2026-10-12 本批 · 真机证据）：`prov.status{connected}` 幂等位的**会话边界复位** ——
#   15:25 第一轮报过一次 connected；15:36 第二轮（App 走 [prov-switch] 断旧连新）拿到 IP 之后
#   **再也没有报** ⇒ App 一直停在"配网中"到超时。成因 = 幂等位跨会话不复位。
#   形状：wifi_prov.c 的 WIFI_EVENT_STA_DISCONNECTED 分支（会话边界）
#         → s_disconnected_cb() → main.c:prov_status_reset_after_disconnect()
#         → 判据本体 wifi_prov_status.h:wifi_prov_connected_reported_after_disconnect()
STATUS_RESET_FN = "wifi_prov_connected_reported_after_disconnect"
DISC_CB_CALL = "s_disconnected_cb();"
DISC_CB_REG = re.compile(r"wifi_prov_set_disconnected_cb\s*\(\s*prov_status_reset_after_disconnect\s*\)")
SUBMIT_SIG = r"esp_err_t\s+wifi_prov_submit_credentials_src\s*\("
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
    # A14d：prov.status{connected} 上报判据（同样只许引用固件那一份）
    "wifi_prov_connected_reportable",
    "wifi_prov_connected_should_report",
    "wifi_prov_connected_reported_after",
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


def strip_comments_keep_newlines(text):
    """去注释（`//` 与块注释）、**保留字符串字面量**、保留换行 ⇒ 行号仍可用。

    ⚠️ A15 必须判定"**代码面**有没有这个锚"：本批在注释里写了旧字逐字留痕
    （`REJECT_ALREADY_UP` / `esp_wifi_disconnect` 等符号名会出现在注释与日志串里），
    不 strip 就会把留痕当实现（诈报）或把日志串当实现（漏判）。
    """
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '"' or c == "'":
            q = c
            out.append(c)
            i += 1
            while i < n:
                out.append(text[i])
                if text[i] == "\\":
                    if i + 1 < n:
                        out.append(text[i + 1])
                        i += 2
                        continue
                elif text[i] == q:
                    i += 1
                    break
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                if text[i] == "\n":
                    out.append("\n")
                i += 1
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def func_body(text, sig_re):
    """按签名正则取函数的**括号配对体**（含两侧花括号）；找不到返回 None。
    ⚠️ 必须带返回类型：注释里也提到过同名函数（只按名字会锚到注释上）。"""
    m = re.search(sig_re, text)
    if m is None:
        return None
    i = text.find("{", m.end())
    if i < 0:
        return None
    depth = 0
    for j in range(i, len(text)):
        if text[j] == "{":
            depth += 1
        elif text[j] == "}":
            depth -= 1
            if depth == 0:
                return text[i:j + 1]
    return None


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

    i_onboarded = policy_text.find("if (onboarded")
    i_held = policy_text.find("if (!held_valid)")
    need(i_onboarded >= 0, "A9c 判据里没有 `if (onboarded` 分支（只入网一次缺失）",
         "见 wifi_prov_policy.h（⚠️ 2026-10-12 起该分支带权威通道排除，见 A15b）")
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
        for tag in ("[夹具①]", "[夹具②]", "[夹具③]", "[夹具④]", "[夹具⑤]", "[夹具⑥]"):
            need(tag in t, "A10c 夹具缺少 {}".format(tag),
                 "六个夹具：SD 胜出 / BLE 胜出 / 重复入网被拒 / prov.status{connected} 上报判据 / "
                 "权威通道断旧连新（2026-10-12）/ 会话边界复位（2026-10-12 本批）")
        need("mutant_decide" in t and "负向对照" in t,
             "A10d 夹具缺少变异负向对照（mutant_decide / 负向对照）",
             "没有负向对照 ⇒ 夹具可能空转")
        need("mutant_decide_reject_up_all" in t,
             "A10f 夹具缺少「把 REJECT_ALREADY_UP 放回」的**专项**负向对照",
             "断旧连新这条裁定必须有可判定的负向对照（变异⑤）")

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

    # ------------------------------------------- A13 资源读数（[res]）必须"未入网也可见"且带阶段标签
    # 形状（2026-10-07 第三批）：同一 helper（全工程**只有 1 处** `[res]` 日志格式串）在 3 个必经点各打一行 ——
    #   ① app_main 内（外设就绪、配网/NimBLE 之前 ⇒ 两机型同阶段可比）
    #   ② wifi_prov_boot 内 wifi_prov_ble_start() **之后**（NimBLE 已起栈、尚未入网 ⇒ BLE 机型的读数；
    #      旧版没有这一处 ⇒ BLE 机型未入网时永远读不到内部 RAM 余量）
    #   ③ cloud_start_task 内（**原有的**那一处，入网后；⛔ 不许被移走/删掉）
    rr = find_calls({"korvo2_oneye_main.c": main_lines}, CALL_RES_REPORT)
    expect(rr, 3, "A13a res_report() 调用点（korvo2_oneye_main.c）",
           "期望 3 处（配网前基线 / NimBLE 后未入网 / 入网后），实测 {} 处".format(len(rr)))
    for fn, ln, func in rr:
        need(func in RES_RESERVED_KINDS,
             "A13a res_report() 出现在 {}()（第 {} 行）".format(func, ln),
             "只允许出现在这 3 个未入网也必经的位置：{}".format("/".join(RES_RESERVED_KINDS)))
    in_app = [h for h in rr if h[2] == "app_main"]
    need(len(in_app) == 1, "A13c app_main() 内的 res_report()",
         "期望恰好 1 处（配网/NimBLE 之前的基线，两机型同阶段可比），实测 {} 处".format(len(in_app)))
    in_cloud = [h for h in rr if h[2] == "cloud_start_task"]
    need(len(in_cloud) == 1, "A13d cloud_start_task() 内的 res_report()（原有读数）",
         "期望恰好 1 处（入网后的量，原有读数不得被删/移），实测 {} 处".format(len(in_cloud)))
    ble_ln = bs[0][1] if bs else None
    if ble_ln is None:
        fails.append("A13b 找不到 wifi_prov_ble_start() 调用点 ⇒ 「NimBLE 起栈后」的读数无从判定")
    else:
        in_ble = [h for h in rr if h[2] == "wifi_prov_boot" and h[1] > ble_ln]
        need(len(in_ble) == 1,
             "A13b wifi_prov_boot() 内 wifi_prov_ble_start()（第 {} 行）之后的 res_report()".format(ble_ln),
             "期望恰好 1 处 —— BLE 机型必须在 NimBLE 起栈后、入网前读到内部 RAM 三个量"
             "（旧版只有入网后那一处 ⇒ 本机型永远读不到），实测 {} 处".format(len(in_ble)))
    fmt = [i + 1 for i, l in enumerate(main_lines) if RES_LOG_FORMAT.search(l)]
    need(len(fmt) == 1, "A13e `[res]` 日志格式串（phase=%s + 三个量）不唯一",
         "实测 {} 处（第 {} 行）—— 多处各写一份会让「同一组量」分叉".format(len(fmt), fmt))
    if len(fmt) == 1:
        for token in ("free_internal=", "largest_internal_block=", "free_psram="):
            need(token in main_lines[fmt[0] - 1], "A13e `[res]` 读数缺少 `{}`".format(token),
                 "三个量必须逐字保留（本批只加 phase 阶段标签，不动量本身）")

    # ------------------------------------------- A14 prov.status{connected,ip} 上报（2026-10-10 本批）
    # 形状：`on_wifi_ready()`（收敛点下游）→ `prov_status_report_connected_once()`
    #        → 判据 `wifi_prov_connected_should_report()`（本体在 wifi_prov_status.h，与夹具同源）
    #        → SDK 既有发送路径 `oneye_dev_link_prov_report_status(ONEYE_DEV_LINK_PROV_CONNECTED, …)`
    # ⛔ 不新增帧 / 不新增错误码：`connected` 是机读契约 `prov_states` 里的**既有**状态。
    status_h = headers.get(STATUS_HEADER, [])
    status_text = "\n".join(status_h)
    need(bool(status_h), "A14d 缺少 main/{}（上报判据的唯一事实源）".format(STATUS_HEADER),
         "缺了它 ⇒ 判据只能散落在 .c 里（固件与夹具会各跑一份）")

    # A14a 上报调用点唯一，且在发帧包装里（link 的 init/start 与 oneye_dev_link.h 都在通道模块；
    #      ⚠️ main.c 结构上不能引该头：`oneye_dev_link_status_t` 在 base/link 两份定义不兼容）
    rp = expect(find_calls(sources, CALL_PROV_REPORT), 1,
                "A14a oneye_dev_link_prov_report_status() 调用点（main/ 下）",
                "⛔ 第二个上报点 = 刷屏或绕过判据")
    if rp:
        if rp[0][2] != STATUS_WRAPPER:
            fails.append("A14a 上报调用点不在 {}() 内（实测在 {}）".format(STATUS_WRAPPER, rp[0][2]))
        if rp[0][0] != STATUS_WRAPPER_FILE:
            fails.append("A14a 上报调用点不在 {} 里（实测在 {}）".format(STATUS_WRAPPER_FILE, rp[0][0]))

    # A14b 上报的状态必须是 connected（⛔ 不许改报别的状态）
    if rp:
        fn, ln, _func = rp[0]
        window = "\n".join(sources[fn][ln - 1:ln + 3])
        need(PROV_CONNECTED_TOKEN.search(window) is not None,
             "A14b 上报的状态不是 ONEYE_DEV_LINK_PROV_CONNECTED（调用点后 4 行内未见该 token）",
             "契约 §4.1：这一帧要落实的是**既有**状态 connected（⛔ 不许换别的状态）")

    # A14c 两级调用点唯一：收敛点 on_wifi_ready() → helper → 发帧包装
    hr = expect(find_calls(sources, CALL_PROV_CONNECTED_REPORT), 1,
                "A14c {}() 调用点".format(STATUS_HELPER),
                "上报必须只在收敛点下游发生一次（⛔ 通道侧不得自建第二条）")
    if hr and hr[0][2] != "on_wifi_ready":
        fails.append("A14c {}() 的调用点不在 on_wifi_ready() 内（实测在 {}）"
                     .format(STATUS_HELPER, hr[0][2]))
    wr = expect(find_calls(sources, CALL_PROV_WRAPPER), 1,
                "A14c {}() 调用点".format(STATUS_WRAPPER),
                "发帧只能由 helper 调一次（⛔ 别处再发 = 绕过判据/刷屏）")
    if wr and wr[0][2] != STATUS_HELPER:
        fails.append("A14c {}() 的调用点不在 {}() 内（实测在 {}）"
                     .format(STATUS_WRAPPER, STATUS_HELPER, wr[0][2]))

    # A14d 判据单源：本体只在 wifi_prov_status.h；调用方只**引用**它（⛔ 不得抄一份）
    need(STATUS_JUDGE_DEF.search(status_text) is not None,
         "A14d {} 的判据本体不在 {}".format("wifi_prov_connected_should_report", STATUS_HEADER),
         "判据必须在纯逻辑头里（固件与主机夹具编译同一份源）")
    judge_calls = find_calls(sources, CALL_STATUS_JUDGE)
    need(len(judge_calls) >= 1, "A14d 没有任何 .c 调用判据 wifi_prov_connected_should_report()",
         "上报前必须过判据（否则等于没有 ip/幂等约束）")
    for fn, lines in sources.items():
        if any(n == "wifi_prov_connected_should_report" for n in defined_functions(lines)):
            fails.append("A14d {} 里自定义了 wifi_prov_connected_should_report()（判据抄件）".format(fn))

    # A14e 「ip 非空」这一条必须在判据里（⛔ 拿不到 IP 不许报 connected）
    need(STATUS_IP_GUARD.search(status_text) is not None,
         "A14e 判据缺少「ip 非空」这一条（ip 为 NULL/空串时的判否）",
         "契约：connected 时 ip 必填；App 判据 = state==\"connected\" && ip 非空")
    # A14f 「只报一次」这一条必须在判据里（⛔ 不刷屏）
    need(STATUS_ONCE_GUARD.search(status_text) is not None,
         "A14f 判据缺少「只报一次」（already_reported）这一条",
         "收敛点每次 GOT_IP 都会被调 ⇒ 没有幂等判据就是刷屏")
    # A14g 幂等位只在发送成功后置位：调用方必须经 wifi_prov_connected_reported_after(…, send_ok)
    mk = find_calls(sources, CALL_STATUS_MARK)
    need(len(mk) == 1, "A14g wifi_prov_connected_reported_after() 调用点",
         "期望恰好 1 处（幂等位只由发送结果推进），实测 {} 处".format(len(mk)))
    if mk:
        need(mk[0][2] == STATUS_HELPER,
             "A14g 幂等位的推进不在 {}() 内（实测在 {}）".format(STATUS_HELPER, mk[0][2]),
             "⛔ 不许在别处置位（例如发送失败也置位 ⇒ App 永远等不到 connected）")
        need("ESP_OK" in sources[mk[0][0]][mk[0][1] - 1],
             "A14g 传给判据的 send_ok 不是发送结果（该行内未见 ESP_OK）",
             "⛔ 不许恒为 true：发失败也必须保持「未报」，否则 App 永远等不到 connected")
    # A14h ⛔ 不新增帧 / 不新增状态
    expect(find_calls(sources, PROV_CONNECTED_TOKEN), 1,
           "A14h main/ 下 ONEYE_DEV_LINK_PROV_CONNECTED 的出现次数",
           "⛔ 本批只落实这一个**既有**状态；多出来的 = 有人新增了状态上报点")
    for fn, lines in sources.items():
        for i, l in enumerate(lines):
            if FRAME_NAME_LITERAL.search(l):
                fails.append("A14h {}:{} 出现了 prov.status 帧名字面量 —— 帧名由 SDK 组，"
                             "设备侧自造帧名就是新增协议面".format(fn, i + 1))

    # ------------------------------------------- A15 权威通道（BLE/App）"断旧连新"（2026-10-12 本批）
    # ⚠️ 全部在 **strip 注释后的代码面** 上判定（注释里的旧字留痕不算实现，日志串也不算）。
    policy_code = strip_comments_keep_newlines(policy_text)
    prov_c_code = strip_comments_keep_newlines(prov_c_text)

    # A15a 判据按通道分派：有权威通道谓词，且**只**对 BLE 为真
    need(AUTH_CHAN_HELPER in policy_code,
         "A15a 缺失 {}".format(AUTH_CHAN_HELPER),
         "权威通道（BLE/App）必须是判据头里的一个显式谓词（按通道分派）")
    auth_body = func_body(policy_code, r"bool\s+" + AUTH_CHAN_HELPER + r"\s*\(") or ""
    if not auth_body:
        fails.append("A15a 找不到 {}() 的函数体（判据不可判定）".format(AUTH_CHAN_HELPER))
    else:
        if "WIFI_PROV_CHAN_BLE" not in auth_body:
            fails.append("A15a {}() 不认 WIFI_PROV_CHAN_BLE ⇒ 权威通道不成立".format(AUTH_CHAN_HELPER))
        others = [c for c in ("WIFI_PROV_CHAN_FILE", "WIFI_PROV_CHAN_API",
                              "WIFI_PROV_CHAN_KCONFIG", "WIFI_PROV_CHAN_SONIC",
                              "WIFI_PROV_CHAN_QRCODE") if c in auth_body]
        if others:
            fails.append("A15a {}() 把非 BLE 通道也算成权威通道（{}）—— ⛔ 只许放宽 C1/C2"
                         .format(AUTH_CHAN_HELPER, ",".join(others)))

    # A15b 「已入网一律拒绝」被权威通道排除 ⇒ REJECT_ALREADY_UP 在 BLE 通路上不可达
    need(AUTH_GUARD in policy_code,
         "A15b 判据里没有 `{}` ⇒ 权威通道仍被「已入网一律拒绝」闸掉（App 卡在配网中的原形态）"
         .format(AUTH_GUARD),
         "`REJECT_ALREADY_UP` 必须**只对非权威通道**可达")
    i_guard = policy_code.find(AUTH_GUARD)
    if i_guard >= 0 and policy_code.find("WIFI_PROV_DECISION_REJECT_ALREADY_UP", i_guard) < 0:
        fails.append("A15b `{}` 之后找不到 `WIFI_PROV_DECISION_REJECT_ALREADY_UP`"
                     .format(AUTH_GUARD))

    # A15c `wifi_prov.c` 的 submit 体内：**恰好一次** esp_wifi_disconnect()，受"在线才断"约束，
    #       且在 `wifi_prov_connect(` 调用**之前**（断旧 ⇒ 连新）
    submit_body = func_body(prov_c_code, SUBMIT_SIG) or ""
    if not submit_body:
        fails.append("A15c 找不到 wifi_prov_submit_credentials_src() 的函数体")
    else:
        i_disc = submit_body.find(DISCONNECT_CALL)
        i_conn = submit_body.find("wifi_prov_connect(")
        i_flag = submit_body.find(REPROVISION_FLAG + " = true")
        ndisc = submit_body.count(DISCONNECT_CALL)
        if ndisc != 1:
            fails.append("A15c submit 路径内 `esp_wifi_disconnect()` 次数 = {}（要求**恰好 1**："
                         "在线才断）".format(ndisc))
        if "if (s_connected)" not in submit_body:
            fails.append("A15c submit 路径内的断链不在 `if (s_connected)` 约束下"
                         "（⚠️ 未在线也会断 ⇒ 不满足「若在线」）")
        if i_disc < 0 or i_conn < 0:
            fails.append("A15c submit 体内缺 `esp_wifi_disconnect()` 或 `wifi_prov_connect(`")
        elif not (i_disc < i_conn):
            fails.append("A15c 顺序错：`esp_wifi_disconnect()` 必须在 `wifi_prov_connect(` "
                         "**之前**（先断旧，再 set_config + connect 连新）")
        # A15d 断链**之前**先置抑制位
        if i_flag < 0:
            fails.append("A15d submit 路径内没有 `{} = true` ⇒ 断链事件会用**旧配置**自动重连"
                         "（换网退化成重连旧网）".format(REPROVISION_FLAG))
        elif i_disc >= 0 and not (i_flag < i_disc):
            fails.append("A15d 顺序错：`{} = true` 必须在 `esp_wifi_disconnect()` **之前**"
                         "（断链事件是异步派发的）".format(REPROVISION_FLAG))
        # A15e 旧采信态作废 + 新通道落槽
        if "s_held_valid = false" not in submit_body:
            fails.append("A15e submit 路径内没有作废旧采信态（缺 `s_held_valid = false`）"
                         "⇒ 旧 SSID 仍被采信")
        if "s_held_chan = ch" not in submit_body:
            fails.append("A15e submit 路径内没有把新通道落槽（缺 `s_held_chan = ch`）")

    # A15d（续）DISCONNECTED 分支必须认抑制位（否则断链即"用旧配置重连"）
    prov_c_lines = sources.get("wifi_prov.c", [])
    ev_body = func_body(strip_comments_keep_newlines("\n".join(prov_c_lines)),
                        r"static\s+void\s+on_wifi_event\s*\(") or ""
    if REPROVISION_FLAG not in ev_body:
        fails.append("A15d on_wifi_event() 的断开分支不认 `{}` ⇒ 断链时仍会用旧配置重连"
                     .format(REPROVISION_FLAG))

    # A15f 通道模块自己不得碰 Wi-Fi 生命周期（esp_wifi_disconnect）
    #   ⚠️ 判定在 **strip 注释后** 的代码面：本批在注释里逐字留痕了旧行为/契约口径，
    #      不 strip 会把留痕当实现（诈报）。
    for fn in CHANNEL_MODULES:
        if fn == "wifi_prov.c":
            continue        # 连接/断链归唯一持有者 wifi_prov.c
        code_lines = strip_comments_keep_newlines("\n".join(sources.get(fn, []))).splitlines()
        for i, line in enumerate(code_lines):
            if "esp_wifi_disconnect" in line:
                fails.append("A15f {}:{} 直接调了 esp_wifi_disconnect()"
                             "（通道只交凭据；Wi-Fi 生命周期归 wifi_prov.c）".format(fn, i + 1))

    # ------------------------------------------- A16 connected 幂等位的**会话边界复位**（🔴 2026-10-12 本批）
    # 真机证据（⛔ 不重复采集）：2026-10-12 15:25 第一轮报过一次 connected；15:36 第二轮
    #   （App 走 [prov-switch] 断旧连新 → GOT_IP 1.3 s、provider 返回 ESP_OK）之后**没有再报**
    #   ⇒ App 一直停在"配网中"到超时。成因 = 幂等位跨会话不复位。
    # 形状（复位点 = **会话边界**，逐字理由见 wifi_prov.h 的 API 注释与 wifi_prov_status.h 的 ④）：
    #   wifi_prov.c:WIFI_EVENT_STA_DISCONNECTED 分支
    #     → s_disconnected_cb() → main.c:prov_status_reset_after_disconnect()
    #     → wifi_prov_status.h:wifi_prov_connected_reported_after_disconnect()
    #   ⛔ 只复位**状态**：判据本体（① ip 非空 / ② 本会话只报一次）一字未改 —— A14e/A14f 仍然断言；
    #   ⛔ 不许把复位挪进上报函数（那等于每次 GOT_IP 都复位 ⇒ 退化成"每次都报"）。
    need(STATUS_RESET_FN in status_text,
         "A16a {}() 的复位跃迁不在 {}".format(STATUS_RESET_FN, STATUS_HEADER),
         "会话边界的复位必须是纯逻辑头里的显式跃迁（固件与主机夹具编译同一份源）")
    reset_body = func_body(status_text, r"bool\s+" + STATUS_RESET_FN + r"\s*\(") or ""
    if not reset_body:
        fails.append("A16a 找不到 {}() 的函数体（复位不可判定）".format(STATUS_RESET_FN))
    else:
        need("return false" in reset_body,
             "A16a {}() 不是真复位（函数体内未见 `return false`）".format(STATUS_RESET_FN),
             "⛔ `return already_reported;` = 永不复位 = 本批要修掉的旧行为（App 卡在配网中）")
        need("return already_reported" not in reset_body,
             "A16a {}() 把入参当返回值（恒等跃迁 = 永不复位）".format(STATUS_RESET_FN),
             "复位与\"之前报没报过\"无关：断开之后一律回到\"未报过\"")

    # A16b 复位边锚在 wifi_prov.c 的 DISCONNECTED 分支（**会话边界**），且只在那里通告一次
    n_cb = prov_c_code.count(DISC_CB_CALL)
    need(n_cb == 1, "A16b wifi_prov.c 里 `{}` 的调用点".format(DISC_CB_CALL),
         "期望恰好 1 处（会话结束的唯一通告点），实测 {} 处".format(n_cb))
    i_disc = prov_c_code.find("WIFI_EVENT_STA_DISCONNECTED")
    i_cb = prov_c_code.find(DISC_CB_CALL)
    i_gotip = prov_c_code.find("IP_EVENT_STA_GOT_IP", i_disc + 1) if i_disc >= 0 else -1
    if i_disc < 0 or i_cb < 0:
        fails.append("A16b 找不到 DISCONNECTED 分支或会话结束通告（复位边不可判定）")
    elif not (i_disc < i_cb):
        fails.append("A16b 会话结束通告不在 WIFI_EVENT_STA_DISCONNECTED 分支内"
                     "（偏移 disc={} cb={}）".format(i_disc, i_cb))
    elif i_gotip >= 0 and not (i_cb < i_gotip):
        fails.append("A16b 会话结束通告越出 DISCONNECTED 分支（已越过 GOT_IP 分支）")

    # A16c 复位**只**由会话结束边推进：⛔ 不许出现在 connected 上报函数里（那会退化成每次都报）
    rc_hits = find_calls(sources, re.compile(STATUS_RESET_FN + r"\s*\("))
    need(len(rc_hits) == 1, "A16c {}() 在 main/ 下的调用点".format(STATUS_RESET_FN),
         "期望恰好 1 处（会话结束回调里复位状态），实测 {} 处".format(len(rc_hits)))
    for fn, ln, func in rc_hits:
        need(func != STATUS_HELPER,
             "A16c {}() 被放进 {}()（{}:{}）".format(STATUS_RESET_FN, STATUS_HELPER, fn, ln),
             "⛔ 每次 GOT_IP 都复位 = 每次都报（刷屏）；复位只能挂在**会话结束**边上")

    # A16d 注册点唯一且在 app_main（与 ready 回调同一处生命周期注册点）
    reg = find_calls(sources, DISC_CB_REG)
    need(len(reg) == 1, "A16d wifi_prov_set_disconnected_cb() 注册点",
         "期望恰好 1 处（会话结束回调），实测 {} 处".format(len(reg)))
    if reg:
        need(reg[0][2] == "app_main",
             "A16d 会话结束回调的注册点不在 app_main() 内（实测在 {}）".format(reg[0][2]),
             "回调注册与 ready 回调同在应用生命周期的唯一注册点")

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
    # A13（2026-10-07 第三批）：把「NimBLE 起栈后、入网前」那一处资源读数删掉 ⇒ 退回"只有入网后可见"。
    ("删掉 NimBLE 起栈后的资源读数（[res] 退回「只有入网后才可见」）", "korvo2_oneye_main.c", "A13",
     lambda ls: _replace_in(ls, 'res_report("ble-ready', 'res_report_disabled("ble-ready')),
    # A14（2026-10-10 本批）：prov.status{connected} 上报的四条硬口径 + 上报点唯一。
    ("丢掉 ip 非空判据（拿不到 IP 也报 connected）", "wifi_prov_status.h", "A14e",
     lambda ls: _replace_in(ls, "if (ip == NULL || ip[0] == '\\0')", "if (false)")),
    ("丢掉幂等判据（每次 GOT_IP 都重报 ⇒ 刷屏）", "wifi_prov_status.h", "A14f",
     lambda ls: _replace_in(ls, "if (already_reported)", "if (false)")),
    ("发送失败也置位（App 永远等不到 connected）", "korvo2_oneye_main.c", "A14g",
     lambda ls: _replace_in(ls, "wifi_prov_connected_reported_after(reported, rc == ESP_OK)",
                            "true")),
    ("第二个 connected 上报点（在别处再报一次）", "korvo2_oneye_main.c", "A14c",
     lambda ls: _append(ls, "\nstatic void injected_second_status_report(void) "
                            "{ prov_status_report_connected_once(); }")),
    ("再开一个绕过判据的直发点（在别的函数里直接调 SDK 发帧）", "wifi_prov_ble.c", "A14a",
     lambda ls: _append(ls, "\nvoid injected_direct_prov_report(void) "
                            "{ (void)oneye_dev_link_prov_report_status("
                            "ONEYE_DEV_LINK_PROV_CONNECTED, NULL, \"1.2.3.4\", NULL, NULL); }")),
    ("把 connected 改报成别的状态", "wifi_prov_ble.c", "A14b",
     lambda ls: _replace_in(ls, "ONEYE_DEV_LINK_PROV_CONNECTED", "ONEYE_DEV_LINK_PROV_SCAN_DONE")),
    # A15（2026-10-12 本批）：权威通道（BLE/App）"断旧连新"的三条硬口径 + 分派本身。
    ("把「已入网一律拒绝」放回（权威通道分派被抹掉）", "wifi_prov_policy.h", "A15b",
     lambda ls: _replace_in(ls, "if (onboarded && !authoritative)", "if (onboarded)")),
    ("删掉断旧前置段（不再 esp_wifi_disconnect()）", "wifi_prov.c", "A15c",
     lambda ls: _replace_in(ls, "esp_err_t drc = esp_wifi_disconnect();",
                            "esp_err_t drc = ESP_OK;")),
    ("抹掉抑制位（断链时会用旧配置自动重连）", "wifi_prov.c", "A15d",
     lambda ls: _replace_in(ls, "if (s_scan_only || s_reprovision)", "if (s_scan_only)")),
    ("抹掉旧采信态作废（旧 SSID 仍被采信）", "wifi_prov.c", "A15e",
     lambda ls: _replace_in(ls, "        s_held_valid = false;",
                            "        /* 注入：不丢旧采信态 */")),
    # A16（🔴 2026-10-12 本批）：会话边界复位 —— 断开边不复位（旧行为）/ 复位被塞进"每次都报"的退化位置。
    ("删掉断开边的会话复位通告（幂等位跨会话永不复位 ⇒ 第二轮配网报不出 connected）",
     "wifi_prov.c", "A16b",
     lambda ls: _replace_in(ls, "            s_disconnected_cb();",
                            "            /* 注入：断开不复位（旧行为） */")),
    ("把复位挪进 connected 上报函数（每次 GOT_IP 都复位 ⇒ 退化成每次都报）",
     "korvo2_oneye_main.c", "A16c",
     lambda ls: _replace_in(
         ls,
         "    if (!wifi_prov_connected_should_report(wifi_prov_is_connected(), ip, reported)) {",
         "    reported = wifi_prov_connected_reported_after_disconnect(reported);\n"
         "    if (!wifi_prov_connected_should_report(wifi_prov_is_connected(), ip, reported)) {")),
    ("把复位跃迁改成恒等（`return already_reported` = 永不复位）", "wifi_prov_status.h", "A16a",
     lambda ls: _replace_in(ls, "    (void)already_reported;",
                            "    return already_reported;   /* 注入：永不复位（旧行为） */")),
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
    for name in ("wifi_prov.h", "wifi_prov_policy.h", "wifi_prov_status.h"):
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
    print("正向断言（A1–A16）：{}".format("PASS" if ok else "FAIL"))

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
    print("RESULT=PASS 单一收敛点 + 优先级（SD>BLE>其他）+ 只入网一次（权威通道 BLE/App 除外）"
          "+ 权威通道断旧连新（A15：恰好一次断链 / 抑制旧配置重连 / 旧 SSID 不再被采信）"
          "+ 自检/授时全通道必经（ADR-0017 D2/D4）+ 资源读数未入网可见（A13，带阶段标签）"
          "+ prov.status{connected,ip} 落地既有契约状态（A14，ip 非空 / 只报一次 / 失败不置位）"
          "+ 会话边界复位（A16：Wi-Fi 断开 ⇒ 重新配网可再报一次；⛔ 不放宽判据、⛔ 不新增帧）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
