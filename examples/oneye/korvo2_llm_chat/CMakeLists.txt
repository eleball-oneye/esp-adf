# =============================================================================
# korvo2_llm_chat —— ESP32-S3-Korvo-2 「长按说话 → 云端 LLM → 喇叭回放」例程
#
# 用法（先导出 IDF，并设 ADF_PATH 指向 esp-adf 检出目录）：
#   export ADF_PATH=<repo>/embedded/esp-adf
#   idf.py set-target esp32s3 && idf.py build
#
# 事实源：
#   - 语音面协议（云端）：backend/contracts/api/ws/语音对话.md + voice-frames.json（oneye.voice.v1）
#   - 本地面协议（配网/链路）：contracts/local/（link-frames.json、ble-gatt.md、smartconfig.md）
#   - 板级音频配方：examples/oneye/korvo2_oneye（同板真机取证过的 AFE/播放配方）
# =============================================================================
cmake_minimum_required(VERSION 3.16)

include($ENV{ADF_PATH}/CMakeLists.txt)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(korvo2_llm_chat)
