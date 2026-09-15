# =============================================================================
# korvo2_oneye —— ESP32-S3-Korvo-2 板级固件（ESP-ADF 例程工程）
#
# 用法（先导出 IDF，并设 ADF_PATH 指向 esp-adf 检出目录）：
#   export ADF_PATH=<repo>/embedded/esp-adf
#   idf.py set-target esp32s3 && idf.py build
#
# 板级硬件参数核对：main/board_expect.h（编译期断言）+ main/korvo2_oneye_main.c（运行期自检）。
# 事实源：docs/zh_CN/design-guide/dev-boards/user-guide-esp32-s3-korvo-2.rst（V3.1）
#        + components/audio_board/esp32_s3_korvo2_v3/{board.c,board_def.h,board_pins_config.c}
# =============================================================================
cmake_minimum_required(VERSION 3.16)

include($ENV{ADF_PATH}/CMakeLists.txt)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(korvo2_oneye)
