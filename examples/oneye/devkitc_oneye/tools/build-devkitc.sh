#!/usr/bin/env bash
# =============================================================================
# build-devkitc.sh —— 两档固件（乙 BETA / 丙 GAMMA）的**受控产出脚本**
#
# ★ 为什么要有脚本而不是手敲 idf.py：本工程有四件"少一件就产出错误固件"的东西
#   （见下），手敲时任何一件被漏掉，构建**照样成功**：
#     ① `export ADF_PATH` —— ⚠️ 缺它会触发组件管理器**删/不解析** managed_components；
#        本工程**无** registry 依赖（`main/idf_component.yml` 为空），但 oneye-dev-sdk
#        住在 `$ADF_PATH/components/` 下 ⇒ ADF_PATH 仍是**组件发现**的必要条件。
#     ② `-DONEYE_SDK_LIB_OUT_DIR=<不存在目录>` —— 源码回落。⚠️ 预编译库归档与源码**不同源**时
#        SDK 的 CMake 会 `FATAL_ERROR`（版本/`src_sha256` 对不上）；本工程按任务口径
#        一律走**源码回落**，且该目录**故意不存在**。
#     ③ `-DSDKCONFIG=<构建目录>/sdkconfig` —— ⛔ 不用工程目录里那份 gitignored `sdkconfig`：
#        IDF 的 SDKCONFIG 缺省落在工程目录，而 defaults 只对"尚未存在的符号"生效
#        ⇒ 工作区那份 sdkconfig 会**盖住**预设，构建还会反过来改写它（"改了 defaults 却不生效"）。
#     ④ `-DSDKCONFIG_DEFAULTS="…;sdkconfig.defaults.<档位>"` —— 档位必须**显式**给；
#        漏了它就回落到 Kconfig 的 choice 缺省 = **甲**（最严）⇒ 编出一个与预期**不同档**的件。
#
# 用法（在**本工程目录**下）：
#   bash tools/build-devkitc.sh beta  <证书目录>   # 乙：证书目录必须含 ca.crt/client.crt/client.key
#   bash tools/build-devkitc.sh gamma <空目录>     # 丙：给一个**空**目录（本档不嵌证书）
#
# 产出（各一档一套 4 个 bin）：见脚本末尾打印的路径。
# ⛔ 本脚本**不烧写**（烧写由 lead 按 README 的四偏移命令执行）。
# =============================================================================
set -euo pipefail

TIER="${1:-}"
CERT_DIR="${2:-}"

if [[ "$TIER" != "beta" && "$TIER" != "gamma" ]]; then
    echo "用法: bash tools/build-devkitc.sh <beta|gamma> <证书目录>" >&2
    exit 2
fi
if [[ -z "$CERT_DIR" ]]; then
    echo "错误：必须显式给出证书目录（乙=非空目录；丙=空目录）—— ⛔ 不提供默认值" >&2
    exit 2
fi

PROJ_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$PROJ_DIR/build-$TIER"
# ⚠️ **故意不存在**：强制 oneye-dev-sdk 走源码回落路径（理由见文件头 ②）。
SDK_LIB_OUT="/nonexistent-oneye-lib-out"

echo "== devkitc_oneye · 档位=$TIER =="
echo "   工程    : $PROJ_DIR"
echo "   构建目录: $BUILD_DIR"
echo "   证书目录: $CERT_DIR"
echo "   ADF_PATH: ${ADF_PATH:-<未导出>}"

if [[ -z "${ADF_PATH:-}" ]]; then
    echo "错误：ADF_PATH 未导出（缺它会触发组件管理器行为差异）—— 请先 export ADF_PATH=<repo>/embedded/esp-adf" >&2
    exit 2
fi
if [[ ! -d "$ADF_PATH/components/oneye-dev-sdk" ]]; then
    echo "错误：$ADF_PATH/components/oneye-dev-sdk 不存在 —— ADF_PATH 指向的不是本仓的 esp-adf" >&2
    exit 2
fi

if [[ "$TIER" == "beta" ]]; then
    for f in ca.crt client.crt client.key; do
        if [[ ! -f "$CERT_DIR/$f" ]]; then
            echo "错误：乙档要求内嵌证书三件齐备，缺 $CERT_DIR/$f" >&2
            exit 2
        fi
    done
else
    if compgen -G "$CERT_DIR/*.crt" > /dev/null 2>&1 || compgen -G "$CERT_DIR/*.key" > /dev/null 2>&1; then
        echo "错误：丙档预期**空**证书目录，但 $CERT_DIR 里有 .crt/.key —— 请换一个空目录（⛔ 不靠"顺手给"绕过档位口径）" >&2
        exit 2
    fi
fi

rm -rf "$BUILD_DIR"

CCACHE_ENABLE=0 idf.py -B "$BUILD_DIR" \
    -DSDKCONFIG="$BUILD_DIR/sdkconfig" \
    -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.defaults.ble;sdkconfig.defaults.$TIER" \
    -DONEYE_FW_CERT_DIR="$CERT_DIR" \
    -DONEYE_SDK_LIB_OUT_DIR="$SDK_LIB_OUT" \
    build

echo
echo "== 产出（$TIER）=="
for f in bootloader/bootloader.bin partition_table/partition-table.bin devkitc_oneye.bin; do
    printf '   %-46s %10s B\n' "$f" "$(stat -c%s "$BUILD_DIR/$f")"
done
