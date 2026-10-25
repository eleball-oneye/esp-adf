# =============================================================================
# thermometer-t31 —— ESP32-S3 × 健奥 T31 体温贴「双角色」例程
#
#   角色（编译期二选一，见 main/Kconfig.projbuild 的 ONEYE_T31_ROLE）：
#     · GATEWAY  网关（COM12）：甲型 OY-GAW-01 —— BLE 中心连 T31 + App BLE 配网 + 上云
#     · SUBDEV   温感子设备模拟器（COM13）：BLE 外设，模拟 T31 广播/GATT/帧
#
# 两个组件（**按路径**挂，不是注册表组件）：
#   · oneye-link-sdk  —— BLE 中心接入（网关用）
#   · oneye-dev-sdk   —— 云链路 / 影子 / 配网件依赖（甲型 creds 分区 + 上云）
#
# ⛔ 本工程**不 include ADF**（与 devkitc_oneye 同口径）：`include($ENV{ADF_PATH}/CMakeLists.txt)`
#    会把 ADF **整棵组件树**挂上来并把 IDF_PATH 换成 ADF 自带 fork；本工程零音频依赖，
#    只把**需要的两个组件**加进 EXTRA_COMPONENT_DIRS。
#
# 构建（WSL，IDF 5.5.5）：
#   source ~/esp/esp-idf-5.5.5/export.sh
#   export ADF_PATH=/mnt/e/workspace/rainmaker-oneye/embedded/esp-adf
#   cd $ADF_PATH/examples/thermometer-t31
#   # 网关（默认角色）
#   idf.py -B build-gw set-target esp32s3 && idf.py -B build-gw build
#   # 子设备（叠加 sdkconfig.defaults.subdev 把角色切成 SUBDEV）
#   idf.py -B build-sub -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.defaults.subdev" set-target esp32s3
#   idf.py -B build-sub build
# =============================================================================
cmake_minimum_required(VERSION 3.16)

# ★ 组件根目录：优先 ADF_PATH（本仓约定），否则按相对路径 ../../ 定位。
# ⚠️ 必须在 `project()` **之前**设置（IDF 在 project() 里才展开组件需求）。
if(DEFINED ENV{ADF_PATH})
    set(_oneye_root "$ENV{ADF_PATH}")
else()
    get_filename_component(_oneye_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
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
# 缺省叠加 `sdkconfig.defaults.esp32s3`（板级：8 MB flash / Octal PSRAM / 自定义分区表）
#
# ⚠️ IDF 5.x **不会**自动把 `sdkconfig.defaults.<target>` 加进 SDKCONFIG_DEFAULTS
#    （实测：`tools/cmake/project.cmake:665-687` 只认 ENV / -D / 缺省那一个文件）。
#    ⇒ 若只写 `sdkconfig.defaults`，`idf.py build` 会掉回 2 MB 缺省 flash 且**没有** creds
#      分区（分区表不进构建）⇒ 甲型必然读不到凭据。故这里给出**可被覆盖**的缺省链：
#      显式 `-DSDKCONFIG_DEFAULTS="…;…;sdkconfig.defaults.subdev"` 时以调用方为准。
# =============================================================================
if(NOT SDKCONFIG_DEFAULTS)
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.esp32s3")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(thermometer_t31)

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
message(STATUS "thermometer_t31: MQTT RX=${CONFIG_ONEYE_FW_MQTT_RX_BUF_BYTES} B / "
               "TXQ=${CONFIG_ONEYE_FW_MQTT_TXQ_BYTES} B（注入 oneye-dev-sdk；⛔ 未改 SDK 源码）")
