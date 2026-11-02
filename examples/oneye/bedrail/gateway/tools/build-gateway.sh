#!/usr/bin/env bash
# =============================================================================
# bedrail/gateway 构建脚本（ESP32-C6-GEEK，COM3）
#
# 用法（先 `source <idf>/export.sh`；未导出 ADF_PATH 时脚本按相对路径取）：
#   bash tools/build-gateway.sh
#
# 为什么必须有脚本（三件"少一件就静默产出错误固件"的事）：
#   ① `-DSDKCONFIG=<build>/sdkconfig` —— 不用工程目录那份 sdkconfig；
#   ② `-DSDKCONFIG_DEFAULTS="…"`    —— IDF 5.5 不会自动叠加 `sdkconfig.defaults.<target>`；
#   ③ `ADF_PATH`                    —— oneye-link-sdk / oneye-dev-sdk 住在 components/ 下。
#
# 产物：build-gw-c6/bedrail_gateway.bin + bootloader.bin + partition_table/
# 分区：factory 0x10000(4M) / model 0x410000(1M) / creds 0x510000(16K) / storage 0x514000(1M)
#       ⛔ creds 偏移不可改（甲型身份在分区里）。
# =============================================================================
set -euo pipefail

PROJ_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJ_DIR"

if [[ -z "${ADF_PATH:-}" ]]; then
    export ADF_PATH="$(cd "$PROJ_DIR/../../../.." && pwd)"
    echo "ADF_PATH 未导出 ⇒ 脚本按相对路径取：$ADF_PATH"
fi

BUILD="build-gw-c6"
DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32c6;sdkconfig.defaults.esp32c6.mem"

echo "== bedrail/gateway · target=esp32c6 · build=$BUILD =="
echo "   SDKCONFIG          = $BUILD/sdkconfig"
echo "   SDKCONFIG_DEFAULTS = $DEFAULTS"

rm -rf "$BUILD"
idf.py -B "$BUILD" -DSDKCONFIG="$BUILD/sdkconfig" \
       -DSDKCONFIG_DEFAULTS="$DEFAULTS" set-target esp32c6
idf.py -B "$BUILD" build

ls -l "$BUILD/bedrail_gateway.bin" "$BUILD/bootloader/bootloader.bin" \
      "$BUILD/partition_table/partition-table.bin"
