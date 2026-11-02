#!/usr/bin/env bash
# =============================================================================
# bedrail/sensor 构建脚本（ESP32-C6-ZERO，COM4）
#
# 用法（先 `source <idf>/export.sh`；未导出 ADF_PATH 时脚本按相对路径取）：
#   bash tools/build-sensor.sh
#
# 本工程是 thermometer-t31 的**完整复制**（子设备角色），默认链 =
#   sdkconfig.defaults
#   ; sdkconfig.defaults.esp32c6
#   ; sdkconfig.defaults.esp32c6.subdev   （8 MB flash / 关 LCD / GNSS / 霍尔）
#   ; sdkconfig.defaults.subdev           （角色 = SUBDEV / 温度周期）
#
# 产物：build-sensor/bedrail_sensor.bin + bootloader.bin + partition_table/
# =============================================================================
set -euo pipefail

PROJ_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJ_DIR"

if [[ -z "${ADF_PATH:-}" ]]; then
    export ADF_PATH="$(cd "$PROJ_DIR/../../../.." && pwd)"
    echo "ADF_PATH 未导出 ⇒ 脚本按相对路径取：$ADF_PATH"
fi

BUILD="build-sensor"
DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32c6;sdkconfig.defaults.esp32c6.subdev;sdkconfig.defaults.subdev"

echo "== bedrail/sensor · target=esp32c6 · build=$BUILD =="
echo "   SDKCONFIG          = $BUILD/sdkconfig"
echo "   SDKCONFIG_DEFAULTS = $DEFAULTS"

rm -rf "$BUILD"
idf.py -B "$BUILD" -DSDKCONFIG="$BUILD/sdkconfig" \
       -DSDKCONFIG_DEFAULTS="$DEFAULTS" set-target esp32c6
idf.py -B "$BUILD" build

ls -l "$BUILD/bedrail_sensor.bin" "$BUILD/bootloader/bootloader.bin" \
      "$BUILD/partition_table/partition-table.bin"
