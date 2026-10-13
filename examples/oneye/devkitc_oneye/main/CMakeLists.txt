# =============================================================================
# devkitc_oneye · main —— 主组件注册
#
# ★ 零音频依赖的**构建期落法**（这是本工程与 korvo2_oneye 最根本的差别）：
#   `REQUIRES` 里**没有** audio_board / audio_hal / audio_pipeline / audio_stream /
#   esp-adf-libs / esp-sr / esp_peripherals / input_key_service / esp32-camera /
#   lcd / spiffs / fatfs。⇒ 本组件的编译单元里**不可能**出现 `board.h` / `es7210.h` /
#   `audio_board_init()` 这类符号（编不过，不是靠人记得）。
#
# 为什么删掉了 korvo2_oneye 的两条 CMake 门禁（⛔ 有意的，不是遗漏）：
#   ①「至少有一条可用配网通道」门禁：它防的是 SD 机型把 WIFI_FILE 与 BLE 都关掉；
#      本工程**只有** BLE 一条通道（无 SD 卡槽）⇒ 判据退化为"BLE 通道必须开"，
#      改由 `main/Kconfig.projbuild` 对 `ONEYE_FW_ONBOARDING_ROLE` 的 `depends on` 承载。
#   ②「REQUIRED=y 与内嵌证书不能同时成立」门禁：**保留**（见下），但**订正了符号名**。
# =============================================================================

# ---- 内嵌证书材料（乙 BETA 的「品类级认证/入网准入前提」载体；⛔ 非一机一密、⛔ 非身份）----
#
# 用 `-DONEYE_FW_CERT_DIR=<目录>` 指定含 ca.crt / client.crt / client.key 的目录（PEM）。
# 三件**齐备**才嵌入；否则 `_oneye_cert_files` 为空 ⇒ 不嵌入、不定义 ONEYE_FW_EMBED_CERTS。
#
# ⚠️ 缺省取 `main/certs`，而本工程的 `main/certs/` **有意不存在**（⟹ 缺省即"不嵌入"）：
#    `korvo2_oneye` 的 certs 目录里躺的是**那台 Korvo-2 的**材料（一机一密），
#    拿它当"品类级证书"用会把两台不同的机器混成一份凭据 —— ⛔ 本工程不留这个口子。
#    ⇒ 乙档的证书**必须由构建命令显式给出**（`-DONEYE_FW_CERT_DIR=<路径>`）。
set(ONEYE_FW_CERT_DIR "${CMAKE_CURRENT_LIST_DIR}/certs" CACHE PATH
    "含 ca.crt/client.crt/client.key 的目录（三件齐备才嵌入；乙档必须显式给出）")

set(_oneye_cert_files "")
if(EXISTS "${ONEYE_FW_CERT_DIR}/ca.crt"
   AND EXISTS "${ONEYE_FW_CERT_DIR}/client.crt"
   AND EXISTS "${ONEYE_FW_CERT_DIR}/client.key")
    set(_oneye_cert_files
        "${ONEYE_FW_CERT_DIR}/ca.crt"
        "${ONEYE_FW_CERT_DIR}/client.crt"
        "${ONEYE_FW_CERT_DIR}/client.key")
    message(STATUS "devkitc_oneye: 嵌入证书材料（ca.crt/client.crt/client.key）⇒ 可作乙档的「品类级入网准入前提」")
else()
    message(STATUS "devkitc_oneye: 未提供证书材料（${ONEYE_FW_CERT_DIR}）⇒ 不嵌入证书（丙档形态；乙档即缺件）")
endif()

idf_component_register(
    SRCS "devkitc_oneye_main.c"
         # 乙档 `prov.pair.ok.sig` 的材料注入点（**单独 TU**：`oneye_dev_ble.h` 与
         # `oneye_dev_link.h` 的 `oneye_dev_link_status_t` 同名不同构，见该文件头）
         "ble_sig_material.c"
         # ---- 从 korvo2_oneye **复制**过来的零音频依赖配网件（⛔ 只读源目录）----
         # BLE 配网通道 C1/C2：档位感知的身份检查 + 配对 + 厂商字段 + 窗口计时 + prov.status 上报
         "wifi_prov_ble.c"
         # Wi-Fi 生命周期 + **入网后收敛点**（唯一 `oneye_start()` 触发边）＋ 优先级判据落点
         "wifi_prov.c"
         # 设备身份的**唯一**读取点（creds 分区的 node_id 与 sn 两串）
         # **单独一个 TU** 的理由（逐字沿用源目录）：`oneye_dev_creds.h` 带进的
         # `oneye_dev_base.h:164` 与 `oneye_dev_link.h:163` 同名不同构
         # （`oneye_dev_link_status_t`）⇒ 同一 TU 同时包含两者必编译失败。
         "adv_identity.c"
         # ★ 2026-10-13〔本批 ③b〕启动期授时（SNTP）—— 从 `korvo2_oneye/main/sntp_boot.c`
         # **同源复制**（逐字节，sha256 `1b02e891…`）。为什么 devkitc 也需要：
         # 乙档是**严格 TLS**（量产档 `CONFIG_ONEYE_FW_TLS_INSECURE=n`），而板子无 RTC
         # ⇒ 不先取钟，服务端证书 notBefore 校验必然 `BADCERT_FUTURE`。
         # 接入点 = `devkitc_oneye_main.c` 的 `on_wifi_ready()`（**在**建上云任务之前）。
         "sntp_boot.c"
    INCLUDE_DIRS "."
    EMBED_FILES ${_oneye_cert_files}
    REQUIRES oneye-dev-sdk
             esp_wifi
             esp_netif
             esp_event
             nvs_flash
             mbedtls
             json
             bt
             driver
             esp_timer
             lwip)

if(_oneye_cert_files)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ONEYE_FW_EMBED_CERTS=1)
endif()

# =============================================================================
# 构建期门禁：`ONEYE_DEV_CREDS_REQUIRED=y` 与"内嵌证书"**不能同时成立**
#
# 两者语义相反：
#   · REQUIRED=y ⇒ 分区凭据是**唯一**来源（分区没写过也不许回退，契约 §7 红线 1）；
#   · 内嵌证书   ⇒ flash 里躺着一份**可回退的公用凭据**。
# 同时成立时，真机表现是"凭证烧录失败的机器照样用公用身份上线"，而平台会把它当成合法设备接受
# ——这是最难在产线上被发现的一类事故。所以这里把它做成**编不过**，而不是靠人记得。
#
# 🔴 **符号名订正（2026-10-10，本工程）**：`korvo2_oneye/main/CMakeLists.txt:116` 写的是
#    `CONFIG_ONEYE_DEV_CREDS_REQUIRED`，而 SDK 的 Kconfig（`components/oneye-dev-sdk/Kconfig:25`）
#    定义的符号是 **`ONEYE_DEV_CREDS_REQUIRED`** ⇒ 生成物里的名字是
#    **`CONFIG_ONEYE_DEV_CREDS_REQUIRED`**。那个门禁因此**从未生效**（恒为假）。
#    本工程按**正确的符号名**书写 ⇒ 门禁真的会拦（负向对照见 README §6）。
#
# ★ 乙 / 丙 两档的**取值口径**：两档都必须 `ONEYE_DEV_CREDS_REQUIRED=n`，
#   因为两者的量产物理事实都是「**无 creds 分区**」（契约 §4.4.8（〇）：乙 / 丙 有品类资格、
#   无实例身份）。⛔ 这不是"放宽"，是**如实配置**：本工程没有一机一密分区可读。
# =============================================================================
if(CONFIG_ONEYE_DEV_CREDS_REQUIRED AND _oneye_cert_files)
    message(FATAL_ERROR
        "自相矛盾的量产配置：ONEYE_DEV_CREDS_REQUIRED=y（分区凭据是唯一来源）"
        " 与内嵌证书（可回退的公用凭据）不能同时成立。\n"
        "  内嵌证书目录：${ONEYE_FW_CERT_DIR}\n"
        "  修法（二选一）：乙 / 丙 档用 -DONEYE_FW_CERT_DIR=<空目录>（本工程缺省即不嵌入）；"
        "或者把 ONEYE_DEV_CREDS_REQUIRED 置 n（乙 / 丙 档的唯一正确取值）。\n"
        "  依据：backend/contracts/domain/设备凭据分区与产测自证契约.md §7 红线 1")
endif()
