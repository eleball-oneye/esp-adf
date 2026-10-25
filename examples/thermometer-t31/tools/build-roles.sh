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
TARGET="${2:-s3}"        # s3（Korvo-2 / 双 S3 联调板，缺省）| c6（微雪 ESP32-C6-GEEK 网关目标板）
PROJ_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJ_DIR"

if [[ -z "${ADF_PATH:-}" ]]; then
    export ADF_PATH="$(cd "$PROJ_DIR/../.." && pwd)"
    echo "ADF_PATH 未导出 ⇒ 本脚本按相对路径取：$ADF_PATH"
fi

build_one() {
    local role="$1"
    local target="$2"
    local build defaults

    case "$role:$target" in
    # ---- ESP32-S3（Korvo-2 网关 / 双 S3 联调）----
    gw:s3)
        build="build-gw"
        defaults="sdkconfig.defaults;sdkconfig.defaults.esp32s3"
        ;;
    sub:s3)
        build="build-sub"
        defaults="sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.defaults.subdev"
        ;;
    # ---- ESP32-C6（微雪 C6-GEEK 网关；子设备可用 C6-ZERO，2026-10-25 起）----
    # `sdkconfig.defaults.esp32c6.mem` 必须排最后：只收紧"看不见"的 WiFi/LWIP/NimBLE/dev-sdk
    # 缓冲（不改 UI、不改业务），用来把断网重连时的 OOM 消掉（见该文件头注释）。
    gw:c6)
        build="build-gw-c6"
        defaults="sdkconfig.defaults;sdkconfig.defaults.esp32c6;sdkconfig.defaults.esp32c6.mem"
        ;;
    # 2026-10-25：COM13 的 S3 板不再插，改用 **COM4 = ESP32-C6-ZERO** 当温感子设备模拟器
    #   ⇒ 子设备角色也要能出 C6 构建。多叠一份 `sdkconfig.defaults.esp32c6.subdev`
    #   （关 LCD、flash 按 8 MB），最后再叠 `sdkconfig.defaults.subdev`（角色/推送周期）。
    sub:c6)
        build="build-sub-c6"
        defaults="sdkconfig.defaults;sdkconfig.defaults.esp32c6;sdkconfig.defaults.esp32c6.subdev;sdkconfig.defaults.subdev"
        ;;
    # ---- C6-GEEK 板载 LCD 背景图（烧到 model 分区，不参与编译）----
    bg:c6|bg:s3)
        build=""
        defaults=""
        ;;
    *)
        echo "用法: bash tools/build-roles.sh <gw|sub|bg|both> [s3|c6]" >&2
        echo "  例: bash tools/build-roles.sh gw          # S3 Korvo-2 网关" >&2
        echo "      bash tools/build-roles.sh gw c6       # ESP32-C6-GEEK 网关（带屏）" >&2
        echo "      bash tools/build-roles.sh sub         # S3 温感子设备模拟器" >&2
        echo "      bash tools/build-roles.sh both        # 两块 S3 全编" >&2
        exit 2
        ;;
    esac

    if [[ "$role" == "bg" ]]; then
        echo "== 背景图不参与编译：用 tools/mk-lcd-bg.py 生成 lcd-bg.bin 后按 0x410000 烧 =="
        return 0
    fi

    echo "== thermometer-t31 · 角色=$role · 目标=$target · 构建目录=$build =="
    echo "   SDKCONFIG         = $build/sdkconfig"
    echo "   SDKCONFIG_DEFAULTS= $defaults"
    rm -rf "$build"
    idf.py -B "$build" -DSDKCONFIG="$build/sdkconfig" -DSDKCONFIG_DEFAULTS="$defaults" set-target "esp32${target}"
    idf.py -B "$build" build
}

case "$ROLE" in
gw|sub) build_one "$ROLE" "$TARGET" ;;
both)
    build_one gw s3
    build_one sub s3
    ;;
*)
    echo "用法: bash tools/build-roles.sh <gw|sub|bg|both> [s3|c6]" >&2
    exit 2
    ;;
esac

echo "== 完成（⛔ 本脚本不烧写）=="
