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
#   bash tools/build-devkitc.sh beta                   # 乙：证书目录缺省 = 本工程 certs-beta/（2026-10-20 起）
#   bash tools/build-devkitc.sh beta  <证书目录>        # 乙：显式指定（须含 ca.crt/client.crt/client.key）
#   bash tools/build-devkitc.sh gamma <空目录>          # 丙：**必须**给一个空目录（本档不嵌证书）
#   bash tools/build-devkitc.sh beta  [<证书目录>] bench # ★ 台面档：再叠加 sdkconfig.defaults.bench-local
#   bash tools/build-devkitc.sh gamma <空目录>   bench   #   （云指本机栈 192.168.110.208:18830 + mbedTLS PSRAM）
#
# ★ 2026-10-20〔栈溢出+证书对齐批〕乙档**新增机内缺省证书目录** `certs-beta/`（不再是"借 korvo2 的 certs"）：
#   · 为什么给缺省：乙档此前每次构建都得手打 `…/korvo2_oneye/main/certs`（别的机型的件），
#     那是 README §7 ⑦ 登记的待裁定项；本批为 devkitc 建了专用目录后，缺省指向它才是"机型事实"。
#   · ⛔ 丙档**仍然**必须显式给空目录：丙不嵌证书，给缺省等于把"这一档不嵌"藏进脚本。
#   · 证书来源 / CN / issuer / 验证读数见 `CERTS-BETA.md`（该目录含私钥 ⇒ gitignore，⛔ 不入库）。
#
# ★ 2026-10-13〔本批〕新增第 3 个参数 `bench`：**只**追加一份预设，且它是**最后一份**
#   （IDF 后叠加者胜 ⇒ 台面档的端点/PSRAM/mbedTLS 覆盖前面的量产口径）。
#   ⛔ 不传 `bench` 时，命令里的 `SDKCONFIG_DEFAULTS` **与从前逐字相同**（4 份）⇒ 量产档零污染。
#   ⚠️ 台面档的**前提**是"本板模组带 PSRAM"（理由见 sdkconfig.defaults.bench-local 文件头）；
#      不带 PSRAM 的模组上，`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y` 会让 TLS 建不起来。
#
# 产出（各一档一套 4 个 bin）：见脚本末尾打印的路径。
# ⛔ 本脚本**不烧写**（烧写由 lead 按 README 的四偏移命令执行）。
# =============================================================================
set -euo pipefail

TIER="${1:-}"
CERT_DIR="${2:-}"
BENCH="${3:-}"

if [[ "$TIER" != "beta" && "$TIER" != "gamma" ]]; then
    echo "用法: bash tools/build-devkitc.sh <beta|gamma> [证书目录] [bench]" >&2
    exit 2
fi
if [[ -n "$BENCH" && "$BENCH" != "bench" ]]; then
    echo "错误：第 3 个参数只能是 \`bench\`（或缺省）—— 实测 '$BENCH'" >&2
    exit 2
fi

PROJ_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# ★ 2026-10-20：乙档的证书目录**有缺省**（本机型专用 `certs-beta/`）；丙档**仍然必须显式**给空目录。
CERT_DIR_DEFAULTED=""
if [[ -z "$CERT_DIR" ]]; then
    if [[ "$TIER" == "beta" ]]; then
        CERT_DIR="$PROJ_DIR/certs-beta"
        CERT_DIR_DEFAULTED=1
    else
        echo "错误：丙档必须显式给出一个**空**证书目录（本档不嵌证书）—— ⛔ 不提供默认值" >&2
        exit 2
    fi
fi
# ★ 台面档用**独立构建目录**（⛔ 不覆盖量产档的 build-<档位>/）：两套件的 sdkconfig/产物并存可对照。
if [[ "$BENCH" == "bench" ]]; then
    BUILD_DIR="$PROJ_DIR/build-$TIER-bench"
else
    BUILD_DIR="$PROJ_DIR/build-$TIER"
fi
# ⚠️ **故意不存在**：强制 oneye-dev-sdk 走源码回落路径（理由见文件头 ②）。
SDK_LIB_OUT="/nonexistent-oneye-lib-out"

echo "== devkitc_oneye · 档位=$TIER${BENCH:+ · 台面档(bench)} =="
echo "   工程    : $PROJ_DIR"
echo "   构建目录: $BUILD_DIR"
echo "   证书目录: $CERT_DIR${CERT_DIR_DEFAULTED:+  ← **机内缺省**（2026-10-20 起；来源见 CERTS-BETA.md）}"
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

# ★ 2026-10-13〔本批〕台面档 = 量产档 4 份预设 ＋ **最后**叠加 sdkconfig.defaults.bench-local。
#   ⛔ 非台面路径的 `-DSDKCONFIG_DEFAULTS` 与从前**逐字相同**（4 份）—— 这是"零污染"的机械证据之一：
#   本文件只在下面这个分支里出现，量产构建命令里 grep 不到它。
if [[ "$BENCH" == "bench" ]]; then
    DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.defaults.ble;sdkconfig.defaults.$TIER;sdkconfig.defaults.bench-local"
    echo "   defaults: $DEFAULTS"
    echo "   ⚠️ 台面档前提：本板模组**必须带 PSRAM**（否则 mbedTLS 外部分配会让 TLS 建不起来）"
else
    DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.defaults.ble;sdkconfig.defaults.$TIER"
    echo "   defaults: $DEFAULTS（量产口径；⛔ 不含 bench-local）"
fi

# ★ 2026-10-20〔影子 JSON 合法性批〕**构建期钉子**（⛔ 不是"顺手多跑一个脚本"）：
#   旧拼装在 `main/devkitc_oneye_main.c` 里产出
#   `{"firmwareVersion":"…","power":true,"credSource":"credSource":"embedded"}}`
#   （重复键 ＋ 多一个右括号）⇒ `oneye_dev_base_report_state()` 判 INVALID **静默失败**，
#   编译照样成功、串口可能一行错都不打，平台侧只是"永远没有 credSource"。
#   ⇒ 在本步拦：模板不可解析 / 结构不符 ⇒ **拒绝构建**（退出码 3，不与 idf.py 的失败混淆）。
#   ⚠️ 判据细节与射程见 `tools/check-shadow-json.py` 文件头（含"只校验拼装模板"的边界声明）。
if ! python3 "$PROJ_DIR/tools/check-shadow-json.py"; then
    echo "错误：影子 reported JSON 不可解析或结构不符 ⇒ 拒绝构建" >&2
    echo "      拼装点：main/devkitc_oneye_main.c 的 snprintf(shadow, …)；判据：tools/check-shadow-json.py" >&2
    exit 3
fi

CCACHE_ENABLE=0 idf.py -B "$BUILD_DIR" \
    -DSDKCONFIG="$BUILD_DIR/sdkconfig" \
    -DSDKCONFIG_DEFAULTS="$DEFAULTS" \
    -DONEYE_FW_CERT_DIR="$CERT_DIR" \
    -DONEYE_SDK_LIB_OUT_DIR="$SDK_LIB_OUT" \
    build
IDF_RC=$?
echo "IDF_EXIT=$IDF_RC"

echo
echo "== 产出（$TIER${BENCH:+ + bench}）=="
for f in bootloader/bootloader.bin partition_table/partition-table.bin devkitc_oneye.bin; do
    printf '   %-46s %10s B\n' "$f" "$(stat -c%s "$BUILD_DIR/$f")"
done
