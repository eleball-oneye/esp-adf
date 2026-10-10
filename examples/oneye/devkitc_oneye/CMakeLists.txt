# =============================================================================
# devkitc_oneye —— ESP32-S3-DevKitC-1 板级固件（**零音频依赖**的 oneye 配网工程）
#
# 定位：把「配网 + 绑定」这条链在本仓**最普通的开发板**（无 codec / 无 LCD / 无摄像头 /
#       无 SD 卡）上做成**可构建、可自证**的产物，用于验证 乙类(BETA) / 丙类(GAMMA)
#       的配网与绑定闭环。
#
# 为什么必须另立工程（⛔ 不是重复建设）：
#   `examples/oneye/korvo2_oneye` **不能直接跑在 DevKitC-1**——
#     · 它的 `app_main()` 无条件调 `audio_board_init()`（ADF 板级，需要 ES8311/ES7210/TCA9554）；
#     · `main/board_expect.h` 是 **Korvo-2 板级参数的编译期断言**（I2S/I2C/PA/按键阈值……）；
#     · `CONFIG_ONEYE_FW_SELFTEST_STRICT=y`（缺省）时板级自检失败即**死等**（`while(1)`）。
#   ⇒ 本工程只保留**零音频依赖**的那一层（配网件 + oneye-dev-sdk），上表的板级件一律不引。
#
# ⛔ 本工程**不 include ADF**：`include($ENV{ADF_PATH}/CMakeLists.txt)` 做了两件事 ——
#    ① `set(EXTRA_COMPONENT_DIRS … $ENV{ADF_PATH}/components)`（把 ADF **整棵组件树**挂上来）；
#    ② `set(ENV{IDF_PATH} "$ENV{ADF_PATH}/esp-idf/")`（⚠️ 把 IDF 换成 ADF 自带的 fork）
#       ＋ 挂上 ADF 的 `project_include.cmake`（它要求 `CONFIG_ESP32_S3_KORVO2_V3_BOARD=y`
#       之类的**板卡选择**）。
#    ⇒ 本工程**两条都不取**，只把**唯一需要的那个组件**（oneye-dev-sdk）单独挂上来。
#
# 🔴 **为什么是"只挂一个组件"而不是"挂 components/ 整目录"（实测踩到，⛔ 别改回去）**：
#    IDF 会把 `EXTRA_COMPONENT_DIRS` 里**每一个**组件都加入构建（不是"按需求拉取"）
#    ⇒ 若写成 `$ENV{ADF_PATH}/components`，ADF 的 `audio_hal` / `esp_peripherals` /
#    `display_service` / `audio_pipeline` … 会被**一起编译**，其中若干用的是
#    FreeRTOS v8 之前的旧类型名（`xSemaphoreHandle` / `xTimerHandle` / `timer_callback`）
#    ⇒ 构建**在 ADF 的音频件上直接失败**（`unknown type name 'xSemaphoreHandle'`），
#    而本工程**一个字节的音频代码都没写**。
#    ⇒ 把路径**收敛到一个组件**，才是"零音频依赖"在构建期的**真正**落法：
#      ADF 的音频件根本不进构建图（不是"编了但没链接"）。
#
# 用法（WSL；先导出 IDF，并按本仓口径导出 ADF_PATH —— ⚠️ 缺 ADF_PATH 会触发组件管理器
# 删 managed_components）：
#   source ~/esp/esp-idf-5.5.5/export.sh
#   export ADF_PATH=/mnt/e/workspace/rainmaker-oneye/embedded/esp-adf
#   cd $ADF_PATH/examples/oneye/devkitc_oneye
#   idf.py set-target esp32s3 && idf.py build
#
# 两档固件的受控产出口径见 README.md 与 `tools/build-devkitc.sh`。
# =============================================================================
cmake_minimum_required(VERSION 3.16)

# ★ 组件搜索路径：**只**加 `oneye-dev-sdk` 这一个组件目录（理由见上）。
# ⚠️ 必须在 `project()` **之前**设置（IDF 在 project() 里才展开组件需求）。
if(NOT DEFINED ENV{ADF_PATH})
    message(FATAL_ERROR
        "ADF_PATH 未导出 ⇒ 找不到本仓组件 oneye-dev-sdk（它在 $ADF_PATH/components/oneye-dev-sdk）。\n"
        "  请先：export ADF_PATH=<repo>/embedded/esp-adf\n"
        "  ⚠️ 缺该变量时组件管理器行为会变（历史事故：删/不解析 managed_components）。")
endif()
set(_oneye_sdk_dir "$ENV{ADF_PATH}/components/oneye-dev-sdk")
if(NOT EXISTS "${_oneye_sdk_dir}/CMakeLists.txt")
    message(FATAL_ERROR
        "在 ${_oneye_sdk_dir} 找不到 oneye-dev-sdk 组件 ⇒ ADF_PATH 指向的不是本仓的 esp-adf。")
endif()
list(APPEND EXTRA_COMPONENT_DIRS "${_oneye_sdk_dir}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(devkitc_oneye)
