# =============================================================================
# bedrail/gateway —— 床护栏网关例程（ESP32-C6-GEEK，COM3）
#
#   角色（编译期，见 main/Kconfig.projbuild 的 ONEYE_T31_ROLE）：
#     · GATEWAY  网关（COM3 / C6-GEEK）：甲型 OY-GAW-01 —— BLE 中心连子设备 +
#                App BLE 配网 + 上云（影子/status）+ event/up 代传 sub.*
#     · SUBDEV   仅保留源码兼容（本工程不构建该角色；床护栏子设备见 ../sensor）
#
#   ★ 本工程复制自 examples/thermometer-t31（同一套 oneye-link-sdk / oneye-dev-sdk
#     接入代码），差异只在：① 默认目标板改为 C6-GEEK；② gw_child.c 允许子设备
#     cmd=0x03 通用 TLV 帧**单独**成为一条 sub.telemetry（霍尔没有温度帧）；
#     ③ 工程身份/版本号改成床护栏口径。
#
# 两个组件（**按路径**挂，不是注册表组件）：
#   · oneye-link-sdk  —— BLE 中心接入（网关用）
#   · oneye-dev-sdk   —— 云链路 / 影子 / 配网件依赖（甲型 creds 分区 + 上云）
#
# ⛔ 本工程**不 include ADF**（与 devkitc_oneye 同口径）：`include($ENV{ADF_PATH}/CMakeLists.txt)`
#    会把 ADF **整棵组件树**挂上来并把 IDF_PATH 换成 ADF 自带 fork；本工程零音频依赖，
#    只把**需要的两个组件**加进 EXTRA_COMPONENT_DIRS。
#
# 构建（Windows / WSL 均可，ESP-IDF 5.5.5）：
#   export ADF_PATH=<repo>/embedded/esp-adf
#   cd $ADF_PATH/examples/oneye/bedrail/gateway
#   bash tools/build-gateway.sh            # 内部 set-target esp32c6 + 三份 defaults
# =============================================================================
cmake_minimum_required(VERSION 3.16)

# ★ 组件根目录：优先 ADF_PATH（本仓约定），否则按相对路径 ../../ 定位。
# ⚠️ 必须在 `project()` **之前**设置（IDF 在 project() 里才展开组件需求）。
if(DEFINED ENV{ADF_PATH})
    set(_oneye_root "$ENV{ADF_PATH}")
else()
    get_filename_component(_oneye_root "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
endif()

set(_oneye_link_dir    "${_oneye_root}/components/oneye-link-sdk")
set(_oneye_dev_sdk_dir "${_oneye_root}/components/oneye-dev-sdk")

if(NOT EXISTS "${_oneye_link_dir}/CMakeLists.txt")
    message(FATAL_ERROR
        "找不到 oneye-link-sdk：${_oneye_link_dir}\n"
        "  → 请设置 ADF_PATH=<repo>/embedded/esp-adf，或确认 ${_oneye_root}/components/ 下该组件存在。")
endif()
if(NOT EXISTS "${_oneye_dev_sdk_dir}/CMakeLists.txt")
    message(FATAL_ERROR
        "找不到 oneye-dev-sdk：${_oneye_dev_sdk_dir}\n"
        "  → 甲型网关要用 creds 分区凭据 + 影子/status 面上云，缺该组件无法构建。\n"
        "  → 请设置 ADF_PATH=<repo>/embedded/esp-adf，或确认组件目录存在。")
endif()

list(APPEND EXTRA_COMPONENT_DIRS "${_oneye_link_dir}" "${_oneye_dev_sdk_dir}")

# =============================================================================
# 缺省叠加 C6-GEEK 板级链（16 MB flash / 自定义分区表 / LCD / C6 内存收敛）
#
# ⚠️ IDF 5.x **不会**自动把 `sdkconfig.defaults.<target>` 加进 SDKCONFIG_DEFAULTS
#    （实测：`tools/cmake/project.cmake:665-687` 只认 ENV / -D / 缺省那一个文件）。
#    ⇒ 若只写 `sdkconfig.defaults`，`idf.py build` 会掉回 2 MB 缺省 flash 且**没有** creds
#      分区（分区表不进构建）⇒ 甲型必然读不到凭据。故这里给出**可被覆盖**的缺省链：
#      显式 `-DSDKCONFIG_DEFAULTS="…"` 时以调用方为准。
# =============================================================================
if(NOT SDKCONFIG_DEFAULTS)
    set(SDKCONFIG_DEFAULTS
        "sdkconfig.defaults;sdkconfig.defaults.esp32c6;sdkconfig.defaults.esp32c6.mem")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(bedrail_gateway)

# =============================================================================
# MQTT 收发缓冲注入（与 devkitc_oneye 逐字同口径，⛔ 不改 SDK 源码）
#
# `components/oneye-dev-sdk/src/internal/oneye_mqtt.c` 里两处 `#ifndef` 守卫缺省
# RX = 65568 B / TXQ = 16384 B —— 本板（BLE 双角色 + TLS + 8 MB PSRAM 但内部堆有限）
# 按用户 2026-10-20 裁定取 **4096 / 4096**（键在 main/Kconfig.projbuild，唯一事实源）。
# ⚠️ 目标名写错必须构建期失败（⛔ 不 `if(TARGET)` 静默跳过：那会让"改了没生效"看起来像"改好了"）。
# =============================================================================
if(NOT TARGET __idf_oneye-dev-sdk)
    message(FATAL_ERROR
        "找不到 SDK 组件目标 `__idf_oneye-dev-sdk` ⇒ MQTT RX/TXQ 编译定义注入点失效。\n"
        "  检查 EXTRA_COMPONENT_DIRS 是否仍把 oneye-dev-sdk 作为组件挂上。")
endif()
target_compile_definitions(__idf_oneye-dev-sdk PRIVATE
    ONEYE_MQTT_RX_BUF_BYTES=${CONFIG_ONEYE_FW_MQTT_RX_BUF_BYTES}
    ONEYE_MQTT_TXQ_BYTES=${CONFIG_ONEYE_FW_MQTT_TXQ_BYTES})
message(STATUS "bedrail_gateway: MQTT RX=${CONFIG_ONEYE_FW_MQTT_RX_BUF_BYTES} B / "
               "TXQ=${CONFIG_ONEYE_FW_MQTT_TXQ_BYTES} B（注入 oneye-dev-sdk；⛔ 未改 SDK 源码）")
