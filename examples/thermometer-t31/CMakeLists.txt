# =============================================================================
# thermometer-t31 —— 微雪 ESP32-C6-GEEK × 健奥 T31 体温贴（oneye-link-sdk 应用层例程）
#
# 定位：最小可跑示例。只依赖 oneye-link-sdk 一个组件，不引入 ESP-ADF 音频件。
# 目标板：ESP32-C6-GEEK（也可直接用于 ESP32-S3 / C3，改 set-target 即可）
#
# 构建（WSL 或已导出 IDF 的 PowerShell）：
#   idf.py set-target esp32c6
#   idf.py build flash monitor
#
# 若未设置 ADF_PATH，本文件按相对路径找组件（../../components/oneye-link-sdk）。
# =============================================================================
cmake_minimum_required(VERSION 3.16)

if(DEFINED ENV{ADF_PATH})
    set(_oneye_link_dir "$ENV{ADF_PATH}/components/oneye-link-sdk")
else()
    get_filename_component(_oneye_link_dir
        "${CMAKE_CURRENT_LIST_DIR}/../../components/oneye-link-sdk" ABSOLUTE)
endif()
if(NOT EXISTS "${_oneye_link_dir}/CMakeLists.txt")
    message(FATAL_ERROR "找不到 oneye-link-sdk：${_oneye_link_dir}
  → 请设置 ADF_PATH=<repo>/embedded/esp-adf，或确认组件目录存在")
endif()
list(APPEND EXTRA_COMPONENT_DIRS "${_oneye_link_dir}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(thermometer_t31)
