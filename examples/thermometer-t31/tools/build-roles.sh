#!/usr/bin/env bash
# =============================================================================
# build-roles.sh —— thermometer-t31 两个角色固件的**受控产出脚本**（⛔ 不烧写、⛔ 不 commit）
#
# 用法（在本工程目录下、已 `source export.sh` 之后）：
#   bash tools/build-roles.sh gw     # 网关（COM12，甲型 OY-GAW-01）
#   bash tools/build-roles.sh sub    # 温感子设备模拟器（COM13，BLE 外设 T31）
#   bash tools/build-roles.sh both
#
# ★ 为什么要有脚本（三件"少一件就静默产出错误固件"的事）：
#   ① `-DSDKCONFIG=<构建目录>/sdkconfig` —— ⛔ 不用工程目录里那份 `sdkconfig`：
#      IDF 的 SDKCONFIG 缺省落在**工程目录**，而 defaults 只对"尚未存在的符号"生效
#      ⇒ 工作区那份 sdkconfig 会**盖住**预设（"改了 defaults 却不生效"），
#      更糟的是两个角色共用同一份 sdkconfig 时，第二个角色会沿用第一个角色的配置。
#   ② `-DSDKCONFIG_DEFAULTS="…"` —— 角色/板级预设必须**显式**给：
#      IDF 5.5 实测（tools/cmake/project.cmake:665-687）**不会**自动叠加
#      `sdkconfig.defaults.<target>`；漏了就掉回 2 MB 缺省 flash 且**没有 creds 分区**。
#   ③ `export ADF_PATH` —— oneye-link-sdk / oneye-dev-sdk 住在 `$ADF_PATH/components/` 下，
#      是本工程**组件发现**的必要条件（缺它 ⇒ 找不到组件 ⇒ CMake FATAL_ERROR）。
#
# 产出（每个角色一套）：
#   build-<role>/thermometer_t31.bin ＋ bootloader.bin ＋ partition_table/partition-table.bin
#   分区表：nvs 0x9000 / phy_init 0xf000 / factory 0x10000(4M) / model 0x410000(1M)
#           creds 0x510000(16K) / storage 0x514000(1M)   ← ⛔ creds 偏移不可改
# =============================================================================
set -euo pipefail

ROLE="${1:-}"
PROJ_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJ_DIR"

if [[ -z "${ADF_PATH:-}" ]]; then
    export ADF_PATH="$(cd "$PROJ_DIR/../.." && pwd)"
    echo "ADF_PATH 未导出 ⇒ 本脚本按相对路径取：$ADF_PATH"
fi

build_one() {
    local role="$1"
    local build defaults

    case "$role" in
    gw)
        build="build-gw"
        defaults="sdkconfig.defaults;sdkconfig.defaults.esp32s3"
        ;;
    sub)
        build="build-sub"
        defaults="sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.defaults.subdev"
        ;;
    *)
        echo "用法: bash tools/build-roles.sh <gw|sub|both>" >&2
        exit 2
        ;;
    esac

    echo "== thermometer-t31 · 角色=$role · 构建目录=$build =="
    echo "   SDKCONFIG         = $build/sdkconfig"
    echo "   SDKCONFIG_DEFAULTS= $defaults"
    rm -rf "$build"
    idf.py -B "$build" -DSDKCONFIG="$build/sdkconfig" -DSDKCONFIG_DEFAULTS="$defaults" set-target esp32s3
    idf.py -B "$build" build
}

case "$ROLE" in
gw|sub) build_one "$ROLE" ;;
both)
    build_one gw
    build_one sub
    ;;
*)
    echo "用法: bash tools/build-roles.sh <gw|sub|both>" >&2
    exit 2
    ;;
esac

echo "== 完成（⛔ 本脚本不烧写）=="
