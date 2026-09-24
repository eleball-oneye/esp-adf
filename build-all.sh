#!/usr/bin/env bash
# =============================================================================
# oneye-dev-sdk 统一编译脚本（位于 esp-adf 仓库根目录）· v0.2（四领域库）
#
# 一次命令、可选工具链，产出「头文件 + 4 个领域库 + 依赖 + demo」，全部落到
# **esp-adf 仓库内的 output/**（已在 .gitignore 中忽略），按工具链名称分目录：
#
#   output/
#   ├── <toolchain-id>/                 # 例：x86_64-linux-gnu-gcc-13.3.0
#   │   ├── include/oneye_dev_*.h            4 个领域库的公共头文件（+ 兼容伞头）
#   │   ├── include/<依赖名>/…               **依赖的第三方头文件**（宿主轨随包复制）
#   │   ├── lib/liboneye_dev_base.a          底座（含内嵌 cJSON 目标文件，符号已加前缀）
#   │   ├── lib/liboneye_dev_{mpp,event,log}.a
#   │   ├── lib/liboneye_dev_*.so*           动态库（仅宿主；DT_NEEDED → base）
#   │   ├── lib/lib<依赖>.a|.so*             **依赖的库**（宿主轨随包复制）
#   │   ├── oneye-dev-sdk.pc                 pkg-config（一次展开 4 个库 + 链接顺序）
#   │   ├── toolchain.json                   构建口径（编译器/版本/IDF/libs[]/vendored[]/承载）
#   │   ├── deps.json                        依赖清单（bundled|provided-by-idf|missing）
#   │   └── SHA256SUMS                       校验值（含随包复制的依赖头文件与库）
#   ├── demo/
#   │   └── <toolchain-id>/               # demo 产物与运行日志（与库分离）
#   │       ├── demo_{base,event,log,mpp,all}(.log)、demo_all_shared(.log)、demo_dlopen(.log)
#   │       ├── oneye_dev_tests(.log)     # 宿主单测
#   │       └── esp-hello_oneye/          # ESP 目标例程产物
#   └── BUILD-REPORT.md                   本次构建汇总（工具链 × 库 × 符号 × 门禁 × 依赖）
#
# **不再产出聚合库 liboneye_dev_sdk.a/.so**（v0.2 起）：合并归档会复制目标文件、无法与分域库共存；
# 应用链接顺序固定 base → mpp → event → log（可用 pkg-config / CMake 目标 oneye-dev-sdk::all 展开）。
#
# 内嵌件（vendor/cjson）：编译后经 objcopy --redefine-syms 加 `oneye_vi_cjson_` 前缀，
# 再合并进 liboneye_dev_base.a —— 交付仍是 4 个库，且与应用自带 cJSON 不会符号重复定义。
#
# 门禁（默认开启，可用 --no-gates 关闭）：
#   * 契约一致性   tools/check-topics.py   —— SDK topic 字面量 ↔ backend asyncapi 通道双向比对
#   * 符号白名单   tools/abi-check.sh      —— 定义符号全为 oneye_*；cJSON_* 零外泄
#   * 宿主单测     oneye_dev_tests         —— 零依赖框架（tests/）
#
# ESP 轨的编译口径（v0.2 起，勿改回手工枚举）：ESP 目标用 **IDF 自身的编译参数**
#   —— 由一个一次性 IDF 探针工程（只 configure）产出 compile_commands.json，
#      再由 tools/idf-flags.py 汇总其 -I/-D/-std/-specs/-m* 等，按工具链缓存于
#      output/.build/idfflags-<tcid>.txt（实测参数记录随产物投放为 <tc>/idf-cflags.txt）。
#   这解决了"未定义 ESP_PLATFORM 导致 oneye_osal_idf.o 编成空目标文件、
#   4 个归档链不上 IDF 工程"的缺陷（详情见 idf_probe_flags 的注释）。
#
# 退出码口径（**失败必须传播到调用者**，勿再出现"编不过却 exit 0"）：
#   0  所有请求的工具链都构建成功；门禁通过；required 依赖齐（若 --deps-strict）；固件轨成功（若 --firmware）。
#      demo / 宿主单测属**辅助轨**：其失败只 warn，不影响退出码（分类口径见「门禁与 demo」一节上方注释）。
#   1  任一工具链的库编译/归档/链接/预编译发布失败、门禁未通过、required 依赖缺失（--deps-strict）、
#      板级固件构建失败、或工具链规格无法识别。stderr 会给出失败摘要（工具链 ID + 失败文件）。
#   2  参数/环境错误（未知参数、SDK 目录无效/缺 src/internal）。
# 失败时的两条硬约束（已实测的真实事故，勿回退）：
#   * 报告照旧生成（失败诊断需要它），但 BUILD-REPORT.md 顶部与文末都显式写明"本次构建失败"与失败清单；
#     收尾横幅不再是"完成。产物树："。
#   * 失败的工具链**不得留下"看起来是新的"产物**：output/<tcid>/ 回滚为上一次产物并写 STALE.md 标注陈旧，
#     本次半成品隔离到 output/.build/failed-<tcid>/；SDK 侧预编译归档 lib/<tcid>/ 同样打陈旧标记。
# 跳过（所请求的工具链在本机不可用：缺 IDF 或交叉编译器）**不计入失败**（`--toolchains all` 会枚举未安装的
#   版本/芯片），但会在 stderr 与报告"已请求但未构建"一节里显式列出，绝不伪装成成功行。
#
# 用法示例（在 esp-adf 根目录执行）：
#   ./build-all.sh --list
#   ./build-all.sh --toolchains host
#   ./build-all.sh --toolchains host,esp32s3@5.5.5,esp32c3@6.0.3
#   ./build-all.sh --toolchains all
#   ./build-all.sh --toolchains host --transport mqtt-ws
#   ./build-all.sh --toolchains host --no-demo --no-tests
#   ./build-all.sh --deps-list
#   ./build-all.sh --no-gates --toolchains host
#   ./build-all.sh --toolchains esp32s3@5.5.5 --firmware     # 额外构建 Korvo-2 板级固件
#
# 固件轨（--firmware，可选；仅对 esp32s3 生效）：
#   ./build-all.sh --toolchains esp32s3@5.5.5 --firmware                        # 缺省 = 量产档位
#   ./build-all.sh --toolchains esp32s3@5.5.5 --firmware --firmware-preset bench # 台面验证档位
#   档位决定叠加哪些 defaults 与是否嵌证书：production 叠加 sdkconfig.defaults.production
#   且不嵌证书（分区凭据是唯一来源，构建期红线守卫会拦下"同时嵌证书"）；bench 只到 esp32s3
#   （台面形态：SD 卡 Wi-Fi 文件、本地验证面），并按需嵌 main/certs 做一机一密自测。
#   产物含 **sdkconfig.txt（这份固件真正生效的全部开关）** 与 **preset-check.txt（逐项核对）**，
#   即"这份固件到底装了什么"可以从产物本身回答，不必靠记忆。
#   构建 examples/oneye/korvo2_oneye（ESP32-S3-Korvo-2 板级固件：板级参数自检 + oneye-dev-sdk 接入），
#   产物落 output/firmware/<toolchain-id>/korvo2_oneye/{.bin,.elf,bootloader,partition-table,build.log,SHA256SUMS}。
#   前置：ADF v2.8 官方仅支持 IDF v5.1–v5.5 ⇒ 请用 esp32s3@5.5.5（6.0.x 会因 esp-sr 依赖 json 而失败）。
# =============================================================================

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

OUT_ROOT=""
SDK_DIR=""
IDF_ROOT="${IDF_ROOT:-$HOME/esp}"
TOOLCHAINS=""
WITH_DEMO=1
WITH_TESTS=1
DO_GATES=1
DO_CLEAN=0
DO_LIST=0
PUBLISH_REPO_LIB=1
DEPS_MODE="auto"
DEPS_STRICT=0
VENDOR_IDF_HEADERS=0
DEPS_LIST_ONLY=0
WITH_FIRMWARE=0
# 固件档位（见 --firmware-preset 的解析处说明）：production = 出货形态（缺省），bench = 台面验证形态
FIRMWARE_PRESET="production"
TRANSPORT="mqtt-tcp,mqtt-tls,mqtt-ws,mqtt-wss"

VENDOR_CJSON_PREFIX="oev_cjson_"
LIBS=(oneye_dev_base oneye_dev_mpp oneye_dev_event oneye_dev_log oneye_dev_link oneye_dev_ble)

C_RESET=$'\033[0m'; C_BOLD=$'\033[1m'; C_GREEN=$'\033[32m'; C_YELLOW=$'\033[33m'; C_RED=$'\033[31m'
# LAST_ERR / LAST_WARN：记录最后一条 err/warn 文本，供工具链成败账本生成"可定位的失败摘要"
# （含工具链 ID 与失败文件）。这些函数是纯输出函数，除记账外不改任何行为。
LAST_ERR=""; LAST_WARN=""
info()  { printf '%s==> %s%s\n' "$C_BOLD" "$*" "$C_RESET"; }
ok()    { printf '    %s%s%s\n' "$C_GREEN" "$*" "$C_RESET"; }
warn()  { printf '    %s%s%s\n' "$C_YELLOW" "$*" "$C_RESET"; LAST_WARN="$*"; }
err()   { printf '%sERROR: %s%s\n' "$C_RED" "$*" "$C_RESET" >&2; LAST_ERR="$*"; }

# 头部注释整块即 --help 的输出（行号随注释增删自动跟随，勿写死）
usage() { sed -n '2,/^# =====/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

# ---------------------------------------------------------------- 参数解析
while [ $# -gt 0 ]; do
    case "$1" in
        -t|--toolchains) TOOLCHAINS="${2:-}"; shift 2 ;;
        --sdk)           SDK_DIR="${2:-}"; shift 2 ;;
        -o|--out)        OUT_ROOT="${2:-}"; shift 2 ;;
        --idf-root)      IDF_ROOT="${2:-}"; shift 2 ;;
        --transport)     TRANSPORT="${2:-}"; shift 2 ;;
        --no-demo)       WITH_DEMO=0; shift ;;
        --no-tests)      WITH_TESTS=0; shift ;;
        --no-gates)      DO_GATES=0; shift ;;
        --no-publish)    PUBLISH_REPO_LIB=0; shift ;;
        --deps)          DEPS_MODE="${2:-auto}"; shift 2 ;;
        --deps-strict)   DEPS_STRICT=1; shift ;;
        --deps-list)     DEPS_LIST_ONLY=1; shift ;;
        --vendor-idf-headers) VENDOR_IDF_HEADERS=1; shift ;;
        --firmware)      WITH_FIRMWARE=1; shift ;;
        --firmware-preset) FIRMWARE_PRESET="${2:-}"; shift 2 ;;
        --no-deps)       DEPS_MODE="none"; shift ;;
        --clean)         DO_CLEAN=1; shift ;;
        --list)          DO_LIST=1; shift ;;
        -h|--help)       usage; exit 0 ;;
        *) err "未知参数：$1"; usage; exit 2 ;;
    esac
done

# 固件档位：production（**缺省**，出货用）/ bench（台面验证用）。
# 缺省选 production 是刻意的：量产固件必须是"默认就能拿到"的那一份，而台面形态要显式要 ——
# 否则"忘了叠加量产预设"就会把台面固件发出去（那正是 2026-09-23 之前的状态）。
case "$FIRMWARE_PRESET" in
    production|bench) ;;
    "") FIRMWARE_PRESET="production" ;;
    *) err "未知 --firmware-preset：$FIRMWARE_PRESET（可选 production | bench）"; exit 2 ;;
esac
if [ "$WITH_FIRMWARE" = "0" ] && [ "$FIRMWARE_PRESET" != "production" ]; then
    warn "--firmware-preset $FIRMWARE_PRESET 只在 --firmware 时生效（本次未构建固件）"
fi

# ------------------------------------------------- SDK 与输出目录自动探测
if [ -z "$SDK_DIR" ]; then
    if [ -f "$SCRIPT_DIR/include/oneye_dev_base.h" ]; then
        SDK_DIR="$SCRIPT_DIR"
    elif [ -f "$SCRIPT_DIR/components/oneye-dev-sdk/include/oneye_dev_base.h" ]; then
        SDK_DIR="$SCRIPT_DIR/components/oneye-dev-sdk"
    elif [ -f "$SCRIPT_DIR/../include/oneye_dev_base.h" ]; then
        SDK_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
    else
        err "未找到 oneye-dev-sdk，请用 --sdk <path> 指定"; exit 2
    fi
fi
[ -f "$SDK_DIR/include/oneye_dev_base.h" ] || { err "SDK 目录无效：$SDK_DIR"; exit 2; }
[ -d "$SDK_DIR/src/internal" ] || { err "SDK 缺少 src/internal/"; exit 2; }

if [ -z "$OUT_ROOT" ]; then
    if [ -f "$SCRIPT_DIR/components/oneye-dev-sdk/include/oneye_dev_base.h" ]; then
        OUT_ROOT="$SCRIPT_DIR/output"
    else
        OUT_ROOT="$PWD/output"
    fi
fi

SDK_VERSION="$(sed -n 's/^#define ONEYE_DEV_SDK_VERSION_STR[[:space:]]*"\(.*\)"/\1/p' "$SDK_DIR/include/oneye_dev_types.h" | head -1)"
SDK_VERSION="${SDK_VERSION:-unknown}"
V_MAJOR="$(sed -n 's/^#define ONEYE_DEV_SDK_VERSION_MAJOR[[:space:]]*\([0-9]*\).*/\1/p' "$SDK_DIR/include/oneye_dev_types.h" | head -1)"
V_MINOR="$(sed -n 's/^#define ONEYE_DEV_SDK_VERSION_MINOR[[:space:]]*\([0-9]*\).*/\1/p' "$SDK_DIR/include/oneye_dev_types.h" | head -1)"
V_PATCH="$(sed -n 's/^#define ONEYE_DEV_SDK_VERSION_PATCH[[:space:]]*\([0-9]*\).*/\1/p' "$SDK_DIR/include/oneye_dev_types.h" | head -1)"
SDK_VERSION_NUM="${V_MAJOR:-0}.${V_MINOR:-0}.${V_PATCH:-0}"

# 源文件清单（库划分的事实源；与 CMakeLists.txt 保持一致）
SRC_INTERNAL="$SDK_DIR/src/internal"
BASE_SRCS=(
    "$SDK_DIR/src/oneye_dev_base.c"
    "$SDK_DIR/src/oneye_dev_creds.c"
    "$SRC_INTERNAL/oneye_internal.c"
    "$SRC_INTERNAL/oneye_test.c"
    "$SRC_INTERNAL/oneye_osal_posix.c"
    "$SRC_INTERNAL/oneye_osal_idf.c"
    "$SRC_INTERNAL/oneye_osal_tls_mbedtls.c"
    # 设备凭据分区平台层（宿主读文件 / ESP 读分区，各自在另一种平台下展开为空）
    "$SDK_DIR/src/oneye_creds_plat_idf.c"
    "$SDK_DIR/src/oneye_creds_plat_posix.c"
    "$SRC_INTERNAL/oneye_util.c"
    "$SRC_INTERNAL/oneye_sha1.c"
    "$SRC_INTERNAL/oneye_json.c"
    "$SRC_INTERNAL/oneye_buf.c"
    "$SRC_INTERNAL/oneye_backoff.c"
    "$SRC_INTERNAL/oneye_topic.c"
    "$SRC_INTERNAL/oneye_envelope.c"
    "$SRC_INTERNAL/oneye_net.c"
    "$SRC_INTERNAL/oneye_mqtt.c"
    "$SRC_INTERNAL/oneye_http.c"
)
MPP_SRCS=( "$SDK_DIR/src/oneye_dev_mpp.c" "$SRC_INTERNAL/oneye_media.c" )
EVENT_SRCS=( "$SDK_DIR/src/oneye_dev_event.c" )
LOG_SRCS=( "$SDK_DIR/src/oneye_dev_log.c" )
# link：设备节点/组域/多信道（本地面）；ble：BLE 配网（自研 GATT）。
# 两者宿主与 ESP 均可编译：ble 的平台实现按 ESP_PLATFORM 在文件内裁剪（nimble 版/桩版）。
LINK_SRCS=(
    "$SDK_DIR/src/oneye_dev_link.c"
    "$SRC_INTERNAL/oneye_link_frame.c"
    "$SRC_INTERNAL/oneye_link_node.c"
)
BLE_SRCS=(
    "$SDK_DIR/src/oneye_dev_ble.c"
    "$SRC_INTERNAL/oneye_ble_pair.c"
    "$SRC_INTERNAL/oneye_ble_plat_nimble.c"
    "$SRC_INTERNAL/oneye_ble_plat_host.c"
)
VENDOR_SRCS=( "$SDK_DIR/vendor/cjson/cJSON.c" )

lib_srcs() {
    local out=""
    case "$1" in
        oneye_dev_base)  out="$(printf '%s\n' "${BASE_SRCS[@]}")" ;;
        oneye_dev_mpp)   out="$(printf '%s\n' "${MPP_SRCS[@]}")" ;;
        oneye_dev_event) out="$(printf '%s\n' "${EVENT_SRCS[@]}")" ;;
        oneye_dev_log)   out="$(printf '%s\n' "${LOG_SRCS[@]}")" ;;
        oneye_dev_link)  out="$(printf '%s\n' "${LINK_SRCS[@]}")" ;;
        oneye_dev_ble)   out="$(printf '%s\n' "${BLE_SRCS[@]}")" ;;
        *) return 0 ;;
    esac
    # 平台适配按目标取舍：宿主用 POSIX（pthread/socket/netdb），ESP 目标用 IDF（FreeRTOS/lwip）。
    # 不依赖文件内的宏开关 —— 交叉编译时 POSIX 头文件根本不存在（如 netdb.h）。
    if [ "${TARGET_IS_ESP:-0}" = "1" ]; then
        printf '%s\n' "$out" | grep -v '/oneye_osal_posix\.c$'
    else
        printf '%s\n' "$out" | grep -v '/oneye_osal_idf\.c$'
    fi
}

# ------------------------------------------------------------ 工具链发现
list_idf_versions() {
    local d
    for d in "$IDF_ROOT"/esp-idf-*; do
        [ -d "$d" ] || continue
        basename "$d" | sed 's/^esp-idf-//'
    done
}

if [ "$DO_LIST" = "1" ]; then
    echo "SDK: $SDK_DIR (version $SDK_VERSION, ${#LIBS[@]} 库：${LIBS[*]})"
    echo "输出根: $OUT_ROOT"
    echo "可用工具链规格："
    printf '  %-18s %s\n' host "宿主编译器（$(cc -dumpmachine 2>/dev/null || echo 'cc 不可用')）"
    for v in $(list_idf_versions); do
        for t in esp32s3 esp32c3 esp32 esp32c6; do
            printf '  %-18s %s\n' "$t@$v" "ESP-IDF v$v（$IDF_ROOT/esp-idf-$v）"
        done
    done
    exit 0
fi

esp_cc_for() {
    local idf="$1" target="$2" ccname kv toolpath p cand
    case "$target" in
        esp32|esp32s2|esp32s3) ccname="xtensa-${target}-elf-gcc" ;;
        *)                     ccname="riscv32-esp-elf-gcc" ;;
    esac
    kv="$(python3 "$idf/tools/idf_tools.py" --idf-path "$idf" export --format key-value 2>/dev/null || true)"
    toolpath="$(printf '%s\n' "$kv" | sed -n 's/^PATH=//p' | head -1)"
    local IFS=':'
    for p in $toolpath; do
        cand="$p/$ccname"
        [ -x "$cand" ] && { echo "$cand"; return 0; }
    done
    command -v "$ccname" 2>/dev/null
}

host_cc() { local c="${CC:-cc}"; command -v "$c" >/dev/null 2>&1 || c=gcc; echo "$c"; }
cc_full_version() { local c="$1" v; v="$("$c" -dumpfullversion 2>/dev/null || true)"; [ -n "$v" ] || v="$("$c" -dumpversion)"; echo "$v"; }

# ======================================================= 依赖解析与打包
DEPS_FAIL=0
deps_conf_for() { case "$1" in host) echo "$SDK_DIR/deps/linux.conf" ;; esp) echo "$SDK_DIR/deps/idf.conf" ;; esac; }

print_deps_list() {
    local kind conf
    for kind in host esp; do
        conf="$(deps_conf_for "$kind")"
        echo "── $kind 轨依赖声明（$conf）"
        if [ -f "$conf" ]; then
            grep -v '^[[:space:]]*#' "$conf" | grep -v '^[[:space:]]*$' | \
                awk -F'|' '{printf "   %-14s %-14s %-9s %s\n", $1, $2, $4, $5}'
        else
            echo "   （未找到声明文件）"
        fi
    done
}

copy_dep_pkgconfig() {
    local out="$1" name="$2" ident="$3" req="$4" desc="$5"
    local ver incd libd libname f
    local -a inc_dirs=() lib_dirs=() lib_names=() hdrs=() libs_copied=()
    ver="$(pkg-config --modversion "$ident")"

    for incd in $(pkg-config --cflags-only-I "$ident"); do inc_dirs+=("${incd#-I}"); done
    incd="$(pkg-config --variable=includedir "$ident" 2>/dev/null || true)"; [ -n "$incd" ] && inc_dirs+=("$incd")
    for incd in $(printf '%s\n' "${inc_dirs[@]:-}" | awk 'NF && !seen[$0]++'); do
        [ -d "$incd" ] || continue
        if [ -d "$incd/$name" ]; then
            mkdir -p "$out/include"
            cp -a "$incd/$name" "$out/include/" 2>/dev/null && hdrs+=("include/$name")
        elif [ "$incd" = "/usr/include" ] || [ "$incd" = "/usr/local/include" ]; then
            warn "  $name：头文件位于系统根 $incd 且无同名子目录（未整体复制）" >&2
        else
            mkdir -p "$out/include/$name"
            cp -a "$incd/." "$out/include/$name/" 2>/dev/null && hdrs+=("include/$name")
        fi
    done

    libd="$(pkg-config --variable=libdir "$ident" 2>/dev/null || true)"; [ -n "$libd" ] && lib_dirs+=("$libd")
    local flag
    for flag in $(pkg-config --libs-only-L --libs-only-l "$ident"); do
        case "$flag" in
            -L*) lib_dirs+=("${flag#-L}") ;;
            -l*) lib_names+=("${flag#-l}") ;;
        esac
    done
    if [ "${#lib_dirs[@]}" -eq 0 ]; then
        lib_dirs=("/usr/lib/$(uname -m)-linux-gnu" "/usr/lib" "/usr/local/lib")
    fi
    mkdir -p "$out/lib"
    for libname in "${lib_names[@]:-}"; do
        [ -n "$libname" ] || continue
        for libd in $(printf '%s\n' "${lib_dirs[@]:-}" | awk 'NF && !seen[$0]++'); do
            for f in "$libd"/lib"$libname".a "$libd"/lib"$libname".so "$libd"/lib"$libname".so.*; do
                [ -e "$f" ] || continue
                cp -a "$f" "$out/lib/" 2>/dev/null && libs_copied+=("lib/$(basename "$f")")
            done
        done
    done
    ok "依赖 $name v$ver：头文件 → include/$name/（${#hdrs[@]} 项）、库 → lib/（${#libs_copied[@]} 个文件）" >&2
    printf '{"name":"%s","kind":"pkgconfig","pkgconfig":"%s","version":"%s","required":"%s","status":"bundled","desc":"%s"}' \
        "$name" "$ident" "$ver" "$req" "$desc"
}

bundle_deps_host() {
    local out="$1" conf; conf="$(deps_conf_for host)"
    mkdir -p "$out/include" "$out/lib"
    if [ "$DEPS_MODE" = "none" ]; then printf '[]\n' > "$out/deps.json"; return 0; fi
    if [ ! -f "$conf" ]; then warn "未找到依赖声明 $conf（跳过依赖打包）"; printf '[]\n' > "$out/deps.json"; return 0; fi
    if ! command -v pkg-config >/dev/null 2>&1; then
        warn "缺少 pkg-config，无法解析依赖（sudo apt-get install -y pkg-config）；deps.json 标记为 missing"
    fi
    local json="[" first=1 name kind ident req desc entry
    while IFS='|' read -r name kind ident req desc; do
        name="$(echo "${name:-}" | xargs)"; kind="$(echo "${kind:-}" | xargs)"
        ident="$(echo "${ident:-}" | xargs)"; req="$(echo "${req:-}" | xargs)"; desc="$(echo "${desc:-}" | xargs)"
        [ -z "$name" ] && continue
        if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists "$ident" 2>/dev/null; then
            entry="$(copy_dep_pkgconfig "$out" "$name" "$ident" "$req" "$desc")"
        else
            entry="{\"name\":\"$name\",\"kind\":\"$kind\",\"pkgconfig\":\"$ident\",\"required\":\"$req\",\"status\":\"missing\",\"desc\":\"$desc\"}"
            if [ "$req" = "required" ]; then
                err "必需依赖缺失：$name（$ident）"
                [ "$DEPS_STRICT" = "1" ] && DEPS_FAIL=1
            else
                warn "依赖缺失（可选）：$name（$ident）"
            fi
        fi
        if [ "$first" = "1" ]; then json="$json$entry"; first=0; else json="$json,$entry"; fi
    done < <(grep -v '^[[:space:]]*#' "$conf" | grep -v '^[[:space:]]*$')
    printf '%s]\n' "$json" > "$out/deps.json"
}

bundle_deps_esp() {
    local out="$1" idf="$2" idfv="${3:-}" conf; conf="$(deps_conf_for esp)"
    if [ "$DEPS_MODE" = "none" ] || [ ! -f "$conf" ]; then printf '[]\n' > "$out/deps.json"; return 0; fi
    local json="[" first=1 name kind req desc cdir ver status entry alt
    while IFS='|' read -r name kind req desc; do
        name="$(echo "${name:-}" | xargs)"; kind="$(echo "${kind:-}" | xargs)"
        req="$(echo "${req:-}" | xargs)"; desc="$(echo "${desc:-}" | xargs)"
        [ -z "$name" ] && continue
        cdir="$idf/components/$name"; ver=""; status="provided-by-idf"
        alt="${name//_/-}"
        [ -d "$cdir" ] || cdir="$idf/components/$alt"
        [ -d "$cdir" ] || cdir="$(find "$idf/components" -maxdepth 3 -type d \( -name "$name" -o -name "$alt" \) 2>/dev/null | head -1)"
        if [ -n "$cdir" ] && [ -f "$cdir/idf_component.yml" ]; then
            ver="$(sed -n 's/^version:[[:space:]]*"\?\([^"]*\)"\?.*/\1/p' "$cdir/idf_component.yml" | head -1)"
        fi
        [ -n "$ver" ] || ver="$idfv"
        if [ -z "$cdir" ] || [ ! -d "$cdir" ]; then
            status="missing"
            if [ "$req" = "required" ]; then
                err "IDF 组件缺失：$name"; [ "$DEPS_STRICT" = "1" ] && DEPS_FAIL=1
            else
                warn "IDF 组件缺失（可选）：$name"
            fi
        else
            if [ "$VENDOR_IDF_HEADERS" = "1" ] && [ -d "$cdir/include" ]; then
                mkdir -p "$out/include/idf-deps/$name"
                cp -a "$cdir/include/." "$out/include/idf-deps/$name/" 2>/dev/null && status="headers-bundled"
            fi
            ok "IDF 依赖 $name${ver:+ v$ver}（$status；库由 IDF 构建系统按 target 提供）"
        fi
        entry="{\"name\":\"$name\",\"kind\":\"idf-component\",\"required\":\"$req\",\"status\":\"$status\",\"version\":\"$ver\",\"desc\":\"$desc\"}"
        if [ "$first" = "1" ]; then json="$json$entry"; first=0; else json="$json,$entry"; fi
    done < <(grep -v '^[[:space:]]*#' "$conf" | grep -v '^[[:space:]]*$')
    printf '%s]\n' "$json" > "$out/deps.json"
}

# ======================================================= 内嵌件符号隔离
# 把 vendor 目标文件的**已定义全局符号**加前缀（不使用 objcopy --prefix-symbols：
# 它会把 memcpy/mbedtls_* 等未定义引用一起改名，导致链接失败）。
#
# 命名口径：`cJSON_Parse` → `${VENDOR_CJSON_PREFIX}Parse`（**去掉原 `cJSON_` 段**，
# 与 src/internal/cJSON.h 的别名表、cmake/vendor_symbols.cmake 完全一致）。
vendor_object_private() { # $1=raw.o  $2=priv.o  $3=nm  $4=objcopy
    local raw="$1" priv="$2" nm_bin="$3" objcopy_bin="$4" redef="${2%.o}.redefine"
    "$nm_bin" -g --defined-only "$raw" | awk '{print $NF}' | \
        grep -E '^[A-Za-z_][A-Za-z0-9_]*$' | sort -u | \
        awk -v p="$VENDOR_CJSON_PREFIX" '{ n=$1; sub(/^cJSON_/, "", n); print $1" "p n }' > "$redef"
    local n; n="$(wc -l < "$redef")"
    if [ "$n" -eq 0 ]; then
        warn "内嵌件未取到符号（$raw）；未做符号隔离" >&2
        cp -f "$raw" "$priv"; return 0
    fi
    "$objcopy_bin" "--redefine-syms=$redef" "$raw" "$priv" || return 1
    ok "内嵌件符号隔离：$n 个符号 → ${VENDOR_CJSON_PREFIX}*"
}

# ======================================================= 失败传播：产物快照与陈旧标注
# 背景（已实测的真实事故，勿回退）：本脚本此前即使编译失败，也照旧打印「完成。产物树：」并重写
# BUILD-REPORT.md，报告里没有任何失败标记；同时 output/<tcid>/ 与 SDK 侧 lib/<tcid>/ 里保留的是
# **上一次**构建的 toolchain.json / lib/*.a（sdk_version 仍是旧值），调用者、CI 和人都会把陈旧归档
# 当成本次结果 —— 实测踩到"预编译归档静默落后于源码、设备端上报了错的 SDK 版本号"。
# 口径：
#   * 失败的工具链**不得产出/刷新** toolchain.json、lib/*.a：构建前把上一次产物挪到
#     output/.build/prev-<tcid>，成功后丢弃快照（成功路径的行为与输出完全不变）；
#     失败则把本次半成品隔离到 output/.build/failed-<tcid>/，并把旧产物原样搬回，
#     再写 STALE.md 明确标注"陈旧、不代表本次结果"。
#   * SDK 侧预编译归档 lib/<tcid>/ 在失败时同样打 STALE.md 陈旧标记；发布成功时自动清除
#     （见 publish_to_repo_lib）—— 这正是堵住"归档静默落后"的那道闸。
TC_SNAP=""
CUR_TCID=""
STALE_DIRS=()

tc_begin() { # $1=tcid $2=out —— 构建前暂存上一次产物
    CUR_TCID="$1"
    TC_SNAP="$OUT_ROOT/.build/prev-$1"
    rm -rf "$TC_SNAP"
    if [ -d "$2" ] && ! mv "$2" "$TC_SNAP"; then
        err "无法暂存旧产物目录：$2"
        TC_SNAP=""
        return 1
    fi
    return 0
}

tc_commit() { # 构建成功：丢弃快照（产物已就位，无需回滚）
    [ -n "$TC_SNAP" ] && rm -rf "$TC_SNAP"
    TC_SNAP=""
    return 0
}

stale_note() { # $1=工具链ID $2=失败原因 —— STALE.md 正文
    printf '# ⚠ 陈旧产物（不是本次构建的结果）\n\n- 工具链：`%s`\n- 本次构建时间：%s（UTC）\n- 失败原因：%s\n\n本目录下的 `toolchain.json` 与 `lib/*.a` 来自**上一次成功的构建**，本次构建**失败**，\n因此它们**没有**被刷新，可能落后于当前 SDK 源码（例如 `sdk_version` 仍是旧值）。\n请勿把本目录当作本次构建的交付物。该工具链重建成功后，本文件会被自动删除。\n' \
        "$1" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$2"
}

mark_repo_lib_stale() { # $1=tcid $2=原因 —— SDK 侧预编译归档打陈旧标记
    local d="$SDK_DIR/lib/$1"
    [ "$PUBLISH_REPO_LIB" = "1" ] || return 0
    [ -f "$d/toolchain.json" ] || return 0
    stale_note "$1" "$2" > "$d/STALE.md" 2>/dev/null || return 0
    warn "预编译归档未刷新（已标注陈旧）：$d/STALE.md"
}

tc_rollback() { # $1=tcid $2=out $3=原因 —— 失败：隔离半成品 + 回滚旧产物 + 标注陈旧
    local tcid="$1" out="$2" reason="$3" quarantined=""
    if [ -n "$TC_SNAP" ]; then
        if [ -d "$out" ]; then
            rm -rf "$OUT_ROOT/.build/failed-$tcid"
            if mv "$out" "$OUT_ROOT/.build/failed-$tcid" 2>/dev/null; then
                quarantined="$OUT_ROOT/.build/failed-$tcid"
            else
                rm -rf "$out"
            fi
        fi
        if [ -d "$TC_SNAP" ]; then
            if mv "$TC_SNAP" "$out" 2>/dev/null; then
                STALE_DIRS+=("$tcid")
                stale_note "$tcid" "$reason" > "$out/STALE.md" 2>/dev/null || true
                warn "已回滚为上一次产物并标注陈旧：$out/STALE.md"
            else
                err "回滚 $tcid 的旧产物失败（快照留在 $TC_SNAP，请人工处理）"
            fi
        fi
        TC_SNAP=""
    fi
    [ -n "$quarantined" ] && warn "本次未完成的半成品已隔离：$quarantined（不要使用）"
    mark_repo_lib_stale "$tcid" "$reason"
    return 0
}

is_stale() { # $1=tcid —— 该工具链本次是否失败（产物已回滚为陈旧）
    local x
    for x in "${STALE_DIRS[@]:-}"; do
        [ "$x" = "$1" ] && return 0
    done
    return 1
}

# ======================================================= 构建：宿主
build_host() {
    local c tcid out scratch nm_bin objcopy_bin
    TARGET_IS_ESP=0
    c="$(host_cc)"
    tcid="$("$c" -dumpmachine)-gcc-$(cc_full_version "$c")"
    out="$OUT_ROOT/$tcid"
    scratch="$OUT_ROOT/.build/host"
    nm_bin="$(command -v nm || echo nm)"
    objcopy_bin="$(command -v objcopy || echo objcopy)"
    # 先暂存上一次产物：本次任一环节失败都会回滚（失败不留"看起来是新的"产物）
    tc_begin "$tcid" "$out" || return 1

    local cflags="-O2 -Wall -Wextra -fPIC -ffunction-sections -fdata-sections -std=c99"
    # 可见性口径（宿主轨）：
    #   base 的目标文件用**默认可见性**——分域 .so（DT_NEEDED → base.so）需要引用 base 的
    #   跨库内部符号（oneye_int_*、oneye_json_*、oneye_topic_* 等），隐藏后无法解析；
    #   分域库自身用 hidden（只导出公共 oneye_dev_*）；
    #   链接 .so 时统一用 version script 限定导出面（见下方 link_shared）。
    local cflags_base="-O2 -Wall -Wextra -fPIC -ffunction-sections -fdata-sections -std=c99"
    local cflags_lib="-O2 -Wall -Wextra -fPIC -fvisibility=hidden -ffunction-sections -fdata-sections -std=c99"
    info "构建宿主工具链：$("$c" --version | head -1)"
    mkdir -p "$out/lib" "$out/include" "$scratch"
    # 先清掉本 SDK 的旧头再复制：`cp -f` 只覆盖、不删除，源码里已删掉的头会一直残留在
    # 产物 include/ 中（实测踩到：include/oneye_dev_sdk_test.h 删除后仍留在产物里，
    # 集成方会拿到源码已不存在的头）。只删 oneye_dev_*.h，依赖子目录（mbedtls/ 等）不受影响。
    rm -f "$out/include"/oneye_dev_*.h
    cp -f "$SDK_DIR"/include/*.h "$out/include/"

    # ① 内嵌件（cJSON）→ 私有符号目标文件
    local vendor_priv_o="$scratch/oneye_vendor_priv.o"
    "$c" $cflags -I"$SDK_DIR/vendor/cjson" -c "${VENDOR_SRCS[0]}" -o "$scratch/oneye_vendor_raw.o" \
        || { err "内嵌件编译失败（$tcid）"; return 1; }
    vendor_object_private "$scratch/oneye_vendor_raw.o" "$vendor_priv_o" "$nm_bin" "$objcopy_bin" \
        || { err "内嵌件符号隔离失败（$tcid）"; return 1; }

    # ② 各领域库：编译 → 归档（base 合并内嵌件）
    #    注意：.so 必须用**目标文件**链接（用归档链 shared 时，ld 只拉取解析未定义符号的成员，
    #    结果会得到一个"空"的 .so），因此这里同时记录每个库的目标文件清单。
    local lib s objs=() o
    declare -A LIB_BODY=()
    declare -A LIB_OBJS=()
    for lib in "${LIBS[@]}"; do
        objs=()
        local _cflags="$cflags_lib"
        [ "$lib" = "oneye_dev_base" ] && _cflags="$cflags_base"
        while IFS= read -r s; do
            [ -n "$s" ] || continue
            [ -f "$s" ] || { err "源文件缺失：$s（$tcid）"; return 1; }
            o="$scratch/$(basename "${s%.c}").o"
            "$c" $_cflags -I"$SDK_DIR/include" -I"$SRC_INTERNAL" -I"$SDK_DIR/vendor/cjson" \
                 -c "$s" -o "$o" || { err "编译失败：$s（$tcid）"; return 1; }
            objs+=("$o")
        done < <(lib_srcs "$lib")
        ar rcs "$scratch/lib${lib}_body.a" "${objs[@]}" \
            || { err "归档失败：$scratch/lib${lib}_body.a（$tcid）"; return 1; }
        LIB_BODY[$lib]="$scratch/lib${lib}_body.a"
        LIB_OBJS[$lib]="${objs[*]}"
        ok "编译 ${lib}（${#objs[@]} 个目标文件）"
    done

    # base = body + 内嵌件（自包含交付单元）
    local ar_out
    ar_out="$({
        echo "CREATE $out/lib/liboneye_dev_base.a"
        echo "ADDLIB ${LIB_BODY[oneye_dev_base]}"
        echo "ADDMOD $vendor_priv_o"
        echo "SAVE"
        echo "END"
    } | ar -M 2>&1)" || { err "合并内嵌件失败（ar -M，$tcid）："; printf '%s\n' "$ar_out" | sed 's/^/    /'; return 1; }
    if [ ! -f "$out/lib/liboneye_dev_base.a" ]; then
        err "合并内嵌件后未生成 liboneye_dev_base.a（$tcid）："; printf '%s\n' "$ar_out" | sed 's/^/    /'; return 1
    fi
    ok "lib/liboneye_dev_base.a（含内嵌 cJSON，符号已隔离） $(stat -c%s "$out/lib/liboneye_dev_base.a") B"

    for lib in oneye_dev_mpp oneye_dev_event oneye_dev_log oneye_dev_link oneye_dev_ble; do
        cp -f "${LIB_BODY[$lib]}" "$out/lib/lib${lib}.a"
        ok "lib/lib${lib}.a $(stat -c%s "$out/lib/lib${lib}.a") B"
    done

    # ③ 动态库（仅宿主；base 打包内嵌件，分域库 DT_NEEDED 依赖 base）
    #
    # 导出面由 version script 限定为 `oneye_*`：公共 API（oneye_dev_*）+ **跨库内部符号**
    # （oneye_int_* / oneye_json_* / oneye_topic_* …，分域 .so 需要它们）；
    # 内嵌件前缀 `oev_`（有意不以 oneye_ 开头）与平台符号（mbedtls_*）一律 local，
    # 保证"内嵌件与第三方符号零外泄"（tools/abi-check.sh 断言）。
    local vermap="$out/oneye-exports.map"
    cat > "$vermap" <<'MAP'
{
  global:
    oneye_*;
  local:
    *;
};
MAP
    local dep_libs; dep_libs="$(pkg-config --libs mbedtls mbedx509 mbedcrypto 2>/dev/null || echo '-lmbedtls -lmbedx509 -lmbedcrypto')"
    "$c" -shared -Wl,-soname,liboneye_dev_base.so.0 -Wl,--version-script="$vermap" \
        -o "$out/lib/liboneye_dev_base.so.$SDK_VERSION_NUM" \
        ${LIB_OBJS[oneye_dev_base]} "$vendor_priv_o" $dep_libs -lpthread -lm \
        || { err "链接 liboneye_dev_base.so 失败（$tcid）"; return 1; }
    for lib in oneye_dev_mpp oneye_dev_event oneye_dev_log oneye_dev_link oneye_dev_ble; do
        "$c" -shared -Wl,-soname,lib${lib}.so.0 -Wl,--version-script="$vermap" \
            -o "$out/lib/lib${lib}.so.$SDK_VERSION_NUM" \
            ${LIB_OBJS[$lib]} -L"$out/lib" -loneye_dev_base -Wl,-rpath,'$ORIGIN' \
            || { err "链接 lib${lib}.so 失败（$tcid）"; return 1; }
    done
    for lib in "${LIBS[@]}"; do
        ln -sf "lib${lib}.so.$SDK_VERSION_NUM" "$out/lib/lib${lib}.so.0"
        ln -sf "lib${lib}.so.0" "$out/lib/lib${lib}.so"
    done
    ok "动态库：4 个 .so.$SDK_VERSION_NUM（含 .so.0/.so 软链）"

    # ④ pkg-config（一次展开 4 库；静态链接需 --start-group —— 分域库依赖 base，
    #    而 GNU ld 处理归档是"单向扫描"，见本文件顶部说明）
    cat > "$out/oneye-dev-sdk.pc" <<EOF
prefix=$out
libdir=\${prefix}/lib
includedir=$SDK_DIR/include

Name: oneye-dev-sdk
Description: oneye 设备侧通信协议 SDK（base/mpp/event/log 四库；EMQX MQTT + WS 承载）
Version: $SDK_VERSION_NUM
Libs: -L\${libdir} -Wl,--start-group -loneye_dev_ble -loneye_dev_link -loneye_dev_mpp -loneye_dev_event -loneye_dev_log -loneye_dev_base -Wl,--end-group
Libs.private: $dep_libs
Cflags: -I\${includedir}
EOF
    ok "oneye-dev-sdk.pc（pkg-config 一次展开 4 库）"

    bundle_deps_host "$out"
    write_manifest "$out" "$tcid" "$c" "$(cc_full_version "$c")" "x86_64/host" "" "$cflags" \
        || { err "生成构建口径清单失败（$tcid：toolchain.json / SHA256SUMS）"; return 1; }
    # 预编译归档发布：**不得吞掉失败**（否则 lib/<tcid>/ 静默落后于源码），见本文件顶部退出码口径
    if [ "$PUBLISH_REPO_LIB" = "1" ]; then
        publish_to_repo_lib "$out" "$tcid" \
            || { err "预编译归档发布失败：lib/$tcid/ 未刷新（$tcid）"; return 1; }
    fi
    LAST_HOST_OUT="$out"
    LAST_HOST_ID="$tcid"
    return 0
}

# =============================================================================
# IDF 真实编译参数（ESP 轨的编译口径来源）
# =============================================================================
# 背景（这是一个已修复的真实缺陷，勿回退）：
#   本脚本的 ESP 轨用独立交叉编译产出 4 个 `.a`。此前这里**没有定义 ESP_PLATFORM**，
#   而 oneye_osal_idf.c 整个文件被 `#if defined(ESP_PLATFORM)` 包裹 →
#   oneye_osal_idf.o 被编成**空目标文件**（实测 712 B、定义符号数 0），
#   于是 4 个 ESP 归档"能编译但永远链不上"，IDF 消费工程报
#   `undefined reference to oneye_osal_sock_recv / _sock_send / _entropy / _now_ms`。
#   同样的开关还会静默关掉 oneye_osal_tls_mbedtls.c 与 oneye_dev_log.c 的 ESP 分支。
#
# 不采用的做法：在 cflags 里手工枚举 IDF 头路径。实测依次踩到
#   sdkconfig.h → FreeRTOSConfig.h → FreeRTOSConfig_arch.h → portmacro.h
#   （`-std=c99` 会关掉 asm，需 gnu99）→ esp_newlib.h →
#   通配 `components/*/include` 把**别的芯片**的 soc 头（esp32c2）抢先命中。
# 采用的做法：让 IDF 自己配置一个一次性探针工程（不编译，只 configure），
#   从它产出的 compile_commands.json 汇总 -I/-D/-std/-m*，按工具链缓存复用。
#   探针组件的 REQUIRES 与 components/oneye-dev-sdk/CMakeLists.txt 的 ESP 组件模式一致，
#   因此拿到的 include 集合就是 SDK 在真实消费工程里的待遇（含 sdkconfig.h 所在目录）。
#
# ⚠️ 已修复的真实缺陷（2026-09-17，勿再漏项）：本清单此前缺 `bt esp_wifi`，于是探针的
#   sdkconfig.h **不含 CONFIG_BT_ENABLED / CONFIG_BT_NIMBLE_ENABLED**，而 oneye_ble_plat_nimble.c
#   整个实现被 `#if defined(ESP_PLATFORM) && defined(CONFIG_BT_ENABLED) && defined(CONFIG_BT_NIMBLE_ENABLED)`
#   包裹 ⇒ 预编译 liboneye_dev_ble.a 里只有宿主桩（oneye_ble_plat_supported() == false），
#   设备上 oneye_dev_ble_init() 恒返回 ERR_UNSUPPORTED（真机取证：korvo2_llm_chat 例程串口 -5）。
#   教训：本清单**必须**与 SDK 组件模式 PRIV_REQUIRES 逐项一致，改一处要改两处。
IDF_PROBE_REQUIRES="esp_netif mbedtls esp_event nvs_flash esp_timer bt esp_wifi"

# 探针口径版本：**改 REQUIRES 或 probe/sdkconfig.defaults 时 +1**，否则旧的 flags 缓存会继续命中，
# 表现为"改了脚本却没生效"（2026-09-17 实测踩到：加了 bt 仍拿到不含 CONFIG_BT_ENABLED 的旧参数）。
IDF_PROBE_REV=2

idf_probe_flags() { # $1=idf 路径 $2=target $3=tcid → 回显 flags 文件路径；失败返回 1
    local idf="$1" target="$2" tcid="$3"
    local cache="$OUT_ROOT/.build/idfflags-$tcid.txt"
    local meta="$cache.meta"
    local probe="$OUT_ROOT/.build/idfprobe-$tcid"
    local want="idf=$idf target=$target probe_rev=$IDF_PROBE_REV"

    # 缓存有效判据：flags 在、stamp 与本次一致、且 sdkconfig.h 仍在（探针目录未被清理）。
    if [ -s "$cache" ] && [ -f "$meta" ] && [ "$(cat "$meta" 2>/dev/null)" = "$want" ] \
       && [ -f "$probe/build/config/sdkconfig.h" ]; then
        echo "$cache"; return 0
    fi

    mkdir -p "$probe/main"
    cat > "$probe/sdkconfig.defaults" <<'PROBE_EOF'
# 探针必须**开启 BLE（NimBLE）**：oneye_ble_plat_nimble.c 的实现体受
#   #if defined(ESP_PLATFORM) && defined(CONFIG_BT_ENABLED) && defined(CONFIG_BT_NIMBLE_ENABLED)
# 保护，IDF 缺省 BT 是关的 ⇒ 关掉时预编译 liboneye_dev_ble.a 只编出宿主桩
# （oneye_ble_plat_supported() == false），设备上 oneye_dev_ble_init() 恒返回 -5 UNSUPPORTED。
CONFIG_BT_ENABLED=y
CONFIG_BT_NIMBLE_ENABLED=y
PROBE_EOF
    # 重要：删掉旧 sdkconfig，否则 `idf.py set-target` 会沿用旧配置、忽略 defaults 的改动。
    rm -f "$probe/sdkconfig"
    cat > "$probe/CMakeLists.txt" <<'PROBE_EOF'
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(oneye_idf_flagprobe)
PROBE_EOF
    cat > "$probe/main/CMakeLists.txt" <<PROBE_EOF
# 探针组件的 REQUIRES 必须与 components/oneye-dev-sdk/CMakeLists.txt（ESP 组件模式）一致，
# 这样 IDF 给出的 include/宏 就是 SDK 源码在真实工程里的那一套。
idf_component_register(SRCS "probe.c" INCLUDE_DIRS "."
                       PRIV_REQUIRES $IDF_PROBE_REQUIRES)
PROBE_EOF
    cat > "$probe/main/probe.c" <<'PROBE_EOF'
/* 探针：只做 include，用于让 IDF 生成"与 SDK 同等待遇"的编译参数。
 * 这里列出的头文件 = oneye_osal_idf.c / oneye_osal_tls_mbedtls.c / oneye_dev_log.c
 * 在 ESP 分支上实际需要的平台头（新增平台依赖时同步补充）。 */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "esp_event.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "mbedtls/base64.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/pk.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "nvs_flash.h"

/* BLE（oneye_ble_plat_nimble.c 的平台头）：列在这里可让"探针未开 BT/NimBLE"当场暴露，
 * 而不是等到设备上 oneye_dev_ble_init() 返回 -5 才发现（见 IDF_PROBE_REQUIRES 的注释）。 */
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

void oneye_idf_flagprobe_touch(void);
void oneye_idf_flagprobe_touch(void) { }
PROBE_EOF

    rm -rf "$probe/build"
    if ! ( set +u; . "$idf/export.sh" >/dev/null 2>&1; \
           idf.py -C "$probe" -B "$probe/build" set-target "$target" ) \
         > "$probe/configure.log" 2>&1; then
        err "IDF 探针配置失败（$target@$(basename "$idf")）：无法取得真实编译参数"
        tail -n 15 "$probe/configure.log" >&2
        return 1
    fi

    local ccjson="$probe/build/compile_commands.json"
    if [ ! -f "$ccjson" ]; then
        err "IDF 未产出 compile_commands.json（$probe/build/）：无法取得真实编译参数"
        return 1
    fi
    if ! python3 "$SCRIPT_DIR/tools/idf-flags.py" --compile-commands "$ccjson" \
             --out "$cache" --source "/main/probe.c" 1>&2; then
        err "提取 IDF 编译参数失败（$ccjson）"
        return 1
    fi
    printf '%s' "$want" > "$meta"
    # 注意：本函数的 stdout **只**回显缓存文件路径，调用方用 $(...) 取它；
    # 一切日志（含 idf-flags.py 的汇总行）都必须走 stderr。
    echo "$cache"
    return 0
}

# ======================================================= 构建：ESP 目标
build_esp() {
    local idfv="$1" target="$2" idf cc tcid out scratch nm_bin objcopy_bin
    TARGET_IS_ESP=1
    idf="$IDF_ROOT/esp-idf-$idfv"
    if [ ! -d "$idf" ]; then warn "跳过 $target@$idfv：未找到 $idf（先安装或改 --idf-root）"; return 2; fi
    cc="$(esp_cc_for "$idf" "$target")"
    if [ -z "$cc" ] || [ ! -x "$cc" ]; then warn "跳过 $target@$idfv：未找到交叉编译器"; return 2; fi
    tcid="$(basename "$cc" | sed 's/-gcc$//')-gcc-$("$cc" -dumpfullversion 2>/dev/null || "$cc" -dumpversion)"
    out="$OUT_ROOT/$tcid"; scratch="$OUT_ROOT/.build/$tcid"
    nm_bin="$(dirname "$cc")/$(basename "$cc" | sed 's/gcc$/nm/')"; [ -x "$nm_bin" ] || nm_bin=nm
    objcopy_bin="$(dirname "$cc")/$(basename "$cc" | sed 's/gcc$/objcopy/')"; [ -x "$objcopy_bin" ] || objcopy_bin=objcopy
    local ar_bin; ar_bin="$(dirname "$cc")/$(basename "$cc" | sed 's/gcc$/ar/')"; [ -x "$ar_bin" ] || ar_bin=ar
    # 先暂存上一次产物：本次任一环节失败都会回滚（失败不留"看起来是新的"产物）
    tc_begin "$tcid" "$out" || return 1

    local arch_flags=""
    case "$target" in esp32|esp32s2|esp32s3) arch_flags="-mlongcalls" ;; esac
    # 平台头文件/宏一律取自 IDF 自身配置（见文件上方 idf_probe_flags 的说明）：
    # 不再手工枚举 mbedtls 布局，也不再手写 ESP_PLATFORM —— 该宏由 IDF 的 -D 一并带入，
    # 这正是本缺陷（OSAL 目标文件为空 → 归档链不上）的修复点。
    local idf_flags_file
    idf_flags_file="$(idf_probe_flags "$idf" "$target" "$tcid")" || return 1
    local -a IDF_FLAGS=()
    local _line
    while IFS= read -r _line; do
        [ -n "$_line" ] && IDF_FLAGS+=("$_line")
    done < "$idf_flags_file"

    # 我们自己的编译选项：语言标准与 -m* 交给 IDF 的参数（见 IDF_FLAGS），
    # 这里只保留 SDK 侧的优化/告警/段裁剪与可见性口径。
    #   -fvisibility=hidden：ESP 轨是静态归档，隐藏内部符号不影响 IDF 链接，
    #                        与宿主轨（.so 由 version script 限面）口径一致；
    #   -ffunction-sections/-fdata-sections：便于消费工程 --gc-sections 裁剪。
    # 有意去掉 -ffreestanding 与 -std=c99：前者不是 IDF 的编译口径（IDF 头依赖 hosted 环境），
    # 后者会把 portmacro.h 的 `asm` 关掉（实测踩过），语言标准改由 IDF 的 -std= 决定。
    local own_flags="-O2 -Wall -Wextra -fno-common -fvisibility=hidden $arch_flags -ffunction-sections -fdata-sections"

    info "构建 $target @ ESP-IDF v$idfv → $tcid"
    mkdir -p "$out/lib" "$out/include" "$scratch"
    # 先清掉本 SDK 的旧头再复制：`cp -f` 只覆盖、不删除，源码里已删掉的头会一直残留在
    # 产物 include/ 中（实测踩到：include/oneye_dev_sdk_test.h 删除后仍留在产物里，
    # 集成方会拿到源码已不存在的头）。只删 oneye_dev_*.h，依赖子目录（mbedtls/ 等）不受影响。
    rm -f "$out/include"/oneye_dev_*.h
    cp -f "$SDK_DIR"/include/*.h "$out/include/"
    # 交付里留一份实际用到的 IDF 参数（消费方/复核用；不含路径以外的敏感信息）
    cp -f "$idf_flags_file" "$out/idf-cflags.txt"

    local vendor_priv_o="$scratch/oneye_vendor_priv.o"
    "$cc" "${IDF_FLAGS[@]}" $own_flags -I"$SDK_DIR/vendor/cjson" -c "${VENDOR_SRCS[0]}" -o "$scratch/oneye_vendor_raw.o" || { err "内嵌件编译失败（$tcid）"; return 1; }
    vendor_object_private "$scratch/oneye_vendor_raw.o" "$vendor_priv_o" "$nm_bin" "$objcopy_bin" \
        || { err "内嵌件符号隔离失败（$tcid）"; return 1; }

    local lib s objs=() o
    declare -A LIB_BODY=()
    for lib in "${LIBS[@]}"; do
        objs=()
        while IFS= read -r s; do
            [ -n "$s" ] || continue
            [ -f "$s" ] || { err "源文件缺失：$s（$tcid）"; return 1; }
            o="$scratch/$(basename "${s%.c}").o"
            # 注意顺序：我们自己的头目录放在 IDF 的 include 之前，避免同名头被 IDF 抢命中
            # （vendor/cjson 的 cJSON.h 与 src/internal/cJSON.h 别名壳必须优先）。
            "$cc" "${IDF_FLAGS[@]}" $own_flags -I"$SDK_DIR/include" -I"$SRC_INTERNAL" -I"$SDK_DIR/vendor/cjson" \
                  -c "$s" -o "$o" || { err "编译失败：$s（$tcid）"; return 1; }
            objs+=("$o")
        done < <(lib_srcs "$lib")
        "$ar_bin" rcs "$scratch/lib${lib}_body.a" "${objs[@]}" \
            || { err "归档失败：$scratch/lib${lib}_body.a（$tcid）"; return 1; }
        LIB_BODY[$lib]="$scratch/lib${lib}_body.a"
    done

    {
        echo "CREATE $out/lib/liboneye_dev_base.a"
        echo "ADDLIB ${LIB_BODY[oneye_dev_base]}"
        echo "ADDMOD $vendor_priv_o"
        echo "SAVE"
        echo "END"
    } | "$ar_bin" -M >/dev/null 2>&1 || { err "合并内嵌件失败（$tcid）"; return 1; }
    ok "lib/liboneye_dev_base.a $(stat -c%s "$out/lib/liboneye_dev_base.a") B"
    for lib in oneye_dev_mpp oneye_dev_event oneye_dev_log oneye_dev_link oneye_dev_ble; do
        cp -f "${LIB_BODY[$lib]}" "$out/lib/lib${lib}.a"
        ok "lib/lib${lib}.a $(stat -c%s "$out/lib/lib${lib}.a") B"
    done

    bundle_deps_esp "$out" "$idf" "$idfv"
    local n_inc n_def idf_std cflags_summary
    n_inc="$(grep -c '^-I' "$idf_flags_file" 2>/dev/null || true)"
    n_def="$(grep -cE '^-[DU]' "$idf_flags_file" 2>/dev/null || true)"
    idf_std="$(grep -m1 '^-std=' "$idf_flags_file" 2>/dev/null || true)"
    cflags_summary="IDF 生成参数 ${idf_std:-（无 -std=）} -I×${n_inc} -D/-U×${n_def}（全文见 idf-cflags.txt） $own_flags"
    write_manifest "$out" "$tcid" "$cc" "$("$cc" -dumpfullversion 2>/dev/null || "$cc" -dumpversion)" "$target" "v$idfv" "$cflags_summary" \
        || { err "生成构建口径清单失败（$tcid：toolchain.json / SHA256SUMS）"; return 1; }
    # 预编译归档发布：**不得吞掉失败**（否则 lib/<tcid>/ 静默落后于源码），见本文件顶部退出码口径
    if [ "$PUBLISH_REPO_LIB" = "1" ]; then
        publish_to_repo_lib "$out" "$tcid" \
            || { err "预编译归档发布失败：lib/$tcid/ 未刷新（$tcid）"; return 1; }
    fi
    ESP_OUTS+=("$tcid|$target|$idfv")
    return 0
}

publish_to_repo_lib() {
    local out="$1" tcid="$2" dest="$SDK_DIR/lib/$tcid" f n=0
    # 原先这里 4 处 `cp ... 2>/dev/null || true` 会把发布失败**全部吞掉**：预编译归档没刷新、
    # toolchain.json 仍是旧版本（sdk_version 对不上源码）却毫无声响 —— 这正是"归档静默落后"
    # 事故的机制。改为逐个校验并返回非零（调用方会判定该工具链构建失败）。
    mkdir -p "$dest" || { err "无法创建预编译归档目录：$dest"; return 1; }
    for f in "$out/lib/"*.a; do
        [ -e "$f" ] || continue
        cp -f "$f" "$dest/" || { err "发布归档失败：$f → $dest/"; return 1; }
        n=$((n + 1))
    done
    [ "$n" -gt 0 ] || { err "发布失败：$out/lib/ 下没有任何 .a 归档"; return 1; }
    for f in "$out/lib/"*.so*; do
        [ -e "$f" ] || continue
        cp -f "$f" "$dest/" || { err "发布动态库失败：$f → $dest/"; return 1; }
    done
    for f in "$out/toolchain.json" "$out/SHA256SUMS"; do
        [ -f "$f" ] || { err "发布失败：缺少 $f"; return 1; }
        cp -f "$f" "$dest/" || { err "发布失败：$f → $dest/"; return 1; }
    done
    rm -f "$dest/STALE.md"   # 本次发布成功 → 陈旧标记自动作废
    return 0
}

# SDK 源内容哈希 —— **必须与组件侧 `cmake/sdk_src_hash.cmake` 用同一口径**（两端比对同一个值）：
#   · 文件集合 = BASE ∪ MPP ∪ EVENT ∪ LOG ∪ LINK ∪ BLE ∪ vendor cJSON ∪ include/oneye_dev_types.h
#     （**平台裁剪之前**的并集）；
#   · 每行 `<basename>:<该文件 sha256>`，按行内容字典序排序，拼成以 `\n` 结尾的文本再取 sha256。
# 用途：ESP-IDF 组件路径在 `lib/<tcid>/` 有归档时**只链接归档、不编译 src/**，而原哨兵只比版本号
# ⇒ 改了源码没改版本号就会静默链接旧库（2026-09-24 为此白烧三轮板子）。消费侧拿这个值比对。
sdk_src_hash() {
    local f base h
    for f in "${BASE_SRCS[@]}" "${MPP_SRCS[@]}" "${EVENT_SRCS[@]}" "${LOG_SRCS[@]}" \
             "${LINK_SRCS[@]}" "${BLE_SRCS[@]}" "${VENDOR_SRCS[@]}" \
             "$SDK_DIR/include/oneye_dev_types.h"; do
        [ -f "$f" ] || continue
        base="$(basename "$f")"
        h="$(sha256sum "$f" | cut -d' ' -f1)"
        printf '%s:%s\n' "$base" "$h"
    done | sort -u | sha256sum | cut -d' ' -f1
}

write_manifest() { # $1=out $2=tcid $3=cc $4=ccver $5=target $6=idfver $7=cflags
    local out="$1" tcid="$2" cc="$3" ccver="$4" target="$5" idfver="$6" cflags="$7"
    local libs_json="" l
    # cflags 里含 `-DNAME="..."` 这类带双引号的宏时，直接插进 JSON 会产生非法 JSON，
    # 因此统一做 JSON 字符串转义（反斜杠与双引号）。
    cflags="$(printf '%s' "$cflags" | sed 's/\\/\\\\/g; s/"/\\"/g')"
    for l in "${LIBS[@]}"; do libs_json="$libs_json\"$l\", "; done
    libs_json="[${libs_json%, }]"
    cat > "$out/toolchain.json" <<EOF
{
  "sdk": "oneye-dev-sdk",
  "sdk_version": "$SDK_VERSION_NUM",
  "sdk_version_str": "$SDK_VERSION",
  "toolchain_id": "$tcid",
  "compiler": "$cc",
  "compiler_version": "$ccver",
  "target": "$target",
  "idf_version": "$idfver",
  "cflags": "$cflags",
  "libs": $libs_json,
  "link_order": "base mpp event log",
  "aggregate_lib": false,
  "vendored": ["cjson(${VENDOR_CJSON_PREFIX})"],
  "faces": ["shadow", "command", "ota", "caps", "status", "event", "track", "log", "mpp"],
  "transport": "$TRANSPORT",
  "contract": "backend/contracts/api/mqtt/asyncapi.yaml v0.2.0 + 传输规范.md",
  "src_sha256": "$(sdk_src_hash)",
  "built_at_utc": "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
}
EOF
    # SHA256SUMS 的生成结果要能反映失败（原实现在 ESP 轨恒返回 1，因为那里不产出 .pc，
    # 只是没有任何调用方看返回值；这里显式返回，便于调用方判定"清单没写成"）。
    local sums_rc=0
    ( cd "$out" && find lib include -type f 2>/dev/null | sort | xargs -r sha256sum > SHA256SUMS ) || sums_rc=1
    if [ -f "$out/oneye-dev-sdk.pc" ]; then
        ( cd "$out" && sha256sum oneye-dev-sdk.pc >> SHA256SUMS ) || sums_rc=1
    fi
    return "$sums_rc"
}

# ======================================================= 门禁与 demo
# 失败分类（改这里之前先读：哪些失败**有意**不影响退出码）：
#   * 致命（→ 退出码 1）：4 个领域库 + link/ble 的编译/归档/链接、构建口径清单（toolchain.json /
#     SHA256SUMS）、**预编译归档发布**（lib/<tcid>/，缺了就是"归档静默落后于源码"）、门禁
#     （run_gates：契约一致性/符号白名单）、required 依赖缺失（--deps-strict）、板级固件、规格写错。
#   * 非致命（有意为之，只 warn）：demo（宿主/板级例程）的编译与运行失败、宿主单测的编译与用例失败。
#     理由：它们是**辅助/示例轨**，其失败不改变交付库的正确性，且单测在迭代期本就常红；
#     把它们判为致命会让日常构建无法进行。若要让单测失败也致命，请另开开关（勿直接改成 err+return 1）。
#   * 非致命但必须可见：所请求工具链在本机不可用（缺 IDF/交叉编译器）＝"跳过"，见主流程账本。
run_gates() { # $1=host out dir
    local out="$1" rc=0
    [ "$DO_GATES" = "1" ] || { warn "门禁已关闭（--no-gates）"; return 0; }

    if command -v python3 >/dev/null 2>&1 && [ -f "$SDK_DIR/tools/check-topics.py" ]; then
        if python3 "$SDK_DIR/tools/check-topics.py" --sdk "$SDK_DIR" 2>&1 | sed 's/^/    /'; then
            ok "契约一致性门禁：通过（SDK topic ↔ backend asyncapi）"
        else
            err "契约一致性门禁失败"; rc=1
        fi
    else
        warn "缺少 python3 或 tools/check-topics.py —— 跳过契约一致性门禁"
    fi

    if [ -f "$SDK_DIR/tools/abi-check.sh" ]; then
        if bash "$SDK_DIR/tools/abi-check.sh" "$out/lib" 2>&1 | sed 's/^/    /'; then
            ok "符号白名单门禁：通过"
        else
            err "符号白名单门禁失败"; rc=1
        fi
    fi

    # 内嵌 cJSON 的**别名完整性**：源码回落路径（在 IDF 里从源码编 SDK）靠别名头给 cJSON 的
    # **定义**改名；别名少一个，该符号就会与应用同时链入的 IDF 自带 `json` 组件
    # `multiple definition of cJSON_*`。而**交付路径（预编译库 + objcopy 全量改名）完全正常**，
    # 所以这个缺陷只在"伙伴从源码构建 SDK"那条路上翻车（2026-09-22 实测：别名表只有 22 个，
    # 实际导出 79 个）。加这条门禁是为了让它不会悄悄退化回去。
    if command -v python3 >/dev/null 2>&1 && [ -f "$SDK_DIR/tools/check-cjson-alias.py" ]; then
        if python3 "$SDK_DIR/tools/check-cjson-alias.py" 2>&1 | sed 's/^/    /'; then
            ok "内嵌 cJSON 别名完整性门禁：通过"
        else
            err "内嵌 cJSON 别名完整性门禁失败"; rc=1
        fi
    else
        warn "缺少 python3 或 tools/check-cjson-alias.py —— 跳过内嵌 cJSON 别名门禁"
    fi
    return $rc
}

build_demo_host() { # $1=out dir
    local out="$1" tcid demo c cflags
    tcid="$(basename "$out")"
    demo="$OUT_ROOT/demo/$tcid"
    [ "$WITH_DEMO" = "1" ] || return 0
    c="$(host_cc)"
    cflags="-O2 -Wall -Wextra -std=c99"
    mkdir -p "$demo"
    info "构建并运行宿主 demo → output/demo/$tcid/"

    # 静态归档需要用 --start-group 包住：分域库引用 base 的内部实现（如 oneye_http_*），
    # GNU ld 对归档是单向扫描，顺序写反或跨库互引都会漏符号。
    local libs="-Wl,--start-group $out/lib/liboneye_dev_ble.a $out/lib/liboneye_dev_link.a $out/lib/liboneye_dev_mpp.a $out/lib/liboneye_dev_event.a $out/lib/liboneye_dev_log.a $out/lib/liboneye_dev_base.a -Wl,--end-group"
    local deplibs; deplibs="$(pkg-config --libs mbedtls mbedx509 mbedcrypto 2>/dev/null || echo '-lmbedtls -lmbedx509 -lmbedcrypto')"
    local name
    for name in base event log mpp all; do
        "$c" $cflags -I"$out/include" "$SDK_DIR/examples/linux/demo_${name}.c" $libs $deplibs -lpthread -lm \
            -o "$demo/demo_${name}" || { err "demo_${name} 编译失败"; continue; }
    done
    "$c" $cflags -I"$out/include" "$SDK_DIR/examples/linux/demo_all.c" \
        -L"$out/lib" -Wl,--start-group -loneye_dev_ble -loneye_dev_link -loneye_dev_mpp -loneye_dev_event -loneye_dev_log -loneye_dev_base -Wl,--end-group \
        -Wl,-rpath,"$out/lib" $deplibs -o "$demo/demo_all_shared" || warn "demo_all_shared 编译失败"
    "$c" $cflags -I"$out/include" "$SDK_DIR/examples/linux/demo_dlopen.c" -ldl \
        -o "$demo/demo_dlopen" || warn "demo_dlopen 编译失败"

    for name in base event log mpp all; do
        [ -x "$demo/demo_${name}" ] || continue
        if "$demo/demo_${name}" > "$demo/demo_${name}.log" 2>&1; then
            ok "demo_${name}（loopback）运行通过 → demo_${name}.log"
        else
            warn "demo_${name} 运行失败（见 demo_${name}.log）"
        fi
    done
    [ -x "$demo/demo_all_shared" ] && { LD_LIBRARY_PATH="$out/lib" "$demo/demo_all_shared" > "$demo/demo_all_shared.log" 2>&1 \
        && ok "demo_all_shared（动态链接 4 个 .so）运行通过" || warn "demo_all_shared 运行失败"; }
    [ -x "$demo/demo_dlopen" ] && { "$demo/demo_dlopen" "$out/lib" > "$demo/demo_dlopen.log" 2>&1 \
        && ok "demo_dlopen（ABI 自检）通过" || warn "demo_dlopen 运行失败"; }
}

build_tests_host() { # $1=out dir
    local out="$1" tcid c demo
    tcid="$(basename "$out")"; demo="$OUT_ROOT/demo/$tcid"
    [ "$WITH_TESTS" = "1" ] || return 0
    [ -d "$SDK_DIR/tests" ] || return 0
    c="$(host_cc)"
    mkdir -p "$demo"
    local srcs=("$SDK_DIR/tests/oneye_test_main.c")
    local f
    for f in "$SDK_DIR"/tests/test_*.c; do [ -f "$f" ] && srcs+=("$f"); done
    local libs="-Wl,--start-group $out/lib/liboneye_dev_ble.a $out/lib/liboneye_dev_link.a $out/lib/liboneye_dev_mpp.a $out/lib/liboneye_dev_event.a $out/lib/liboneye_dev_log.a $out/lib/liboneye_dev_base.a -Wl,--end-group"
    local deplibs; deplibs="$(pkg-config --libs mbedtls mbedx509 mbedcrypto 2>/dev/null || echo '-lmbedtls -lmbedx509 -lmbedcrypto')"
    info "构建并运行宿主单测（tests/）"
    if "$c" -O1 -g -Wall -Wextra -std=c99 -I"$out/include" -I"$SRC_INTERNAL" -I"$SDK_DIR/tests" \
        -I"$SDK_DIR/vendor/cjson" -DONEYE_TEST_VECTORS_DIR="\"$SDK_DIR/tests/vectors\"" \
        -DONEYE_TEST_FIXTURES_DIR="\"$SDK_DIR/tests/fixtures\"" \
        "${srcs[@]}" $libs $deplibs -lpthread -lm -o "$demo/oneye_dev_tests" 2>"$demo/oneye_dev_tests.build.log"; then
        if "$demo/oneye_dev_tests" > "$demo/oneye_dev_tests.log" 2>&1; then
            ok "oneye_dev_tests 全部通过 → demo/oneye_dev_tests.log"
        else
            warn "oneye_dev_tests 存在失败（见 demo/oneye_dev_tests.log）"
        fi
    else
        warn "单测编译失败（见 demo/oneye_dev_tests.build.log）"
    fi
}

build_demo_esp() { # $1=idf 版本, $2=target
    local idfv="$1" target="$2" idf cc tcid demo ex
    [ "$WITH_DEMO" = "1" ] || return 0
    idf="$IDF_ROOT/esp-idf-$idfv"; [ -d "$idf" ] || return 0
    cc="$(esp_cc_for "$idf" "$target")"; [ -n "$cc" ] || return 0
    tcid="$(basename "$cc" | sed 's/-gcc$//')-gcc-$("$cc" -dumpversion)"
    demo="$OUT_ROOT/demo/$tcid"; mkdir -p "$demo"
    ex="$SDK_DIR/examples/esp-idf/hello_oneye"
    info "构建板级例程 hello_oneye（$target @ IDF v$idfv）→ output/demo/$tcid/esp-hello_oneye/"
    local build_dir="$OUT_ROOT/.build/esp-hello_oneye-$tcid"
    (
        set +u
        . "$idf/export.sh" >/dev/null 2>&1
        export ONEYE_DEV_SDK_PATH="$SDK_DIR"
        cd "$ex"
        rm -rf "$build_dir"
        idf.py -B "$build_dir" set-target "$target" > "$demo/esp-build.log" 2>&1 \
            && idf.py -B "$build_dir" build >> "$demo/esp-build.log" 2>&1
    ) || { warn "hello_oneye 构建失败（见 esp-build.log）"; return 2; }
    mkdir -p "$demo/esp-hello_oneye"
    cp -f "$build_dir"/hello_oneye.bin "$build_dir"/hello_oneye.elf "$build_dir"/bootloader/bootloader.bin \
          "$build_dir"/partition_table/partition-table.bin "$demo/esp-hello_oneye/" 2>/dev/null || true
    cp -f "$demo/esp-build.log" "$demo/esp-hello_oneye/" 2>/dev/null || true
    if grep -q "链接预编译库" "$demo/esp-build.log" 2>/dev/null; then
        ok "hello_oneye 构建通过；组件自动链接 4 个预编译库"
    else
        warn "hello_oneye 构建完成，但日志未见 '链接预编译库'（可能回落源码构建）"
    fi
}

# 固件"装了哪些模块/哪些开关生效"的**人读版清单**（产物 modules.txt）。
# 判据全部取自**这次构建的真实产物**（bdir/sdkconfig + 被链接的预编译归档 + 板级文件），
# 不是把 defaults 里的意图复述一遍 —— 这两者在历史上长得一样，而只有前者是真的。
firmware_modules_manifest() { # $1=产物目录 $2=档位 $3=工具链 id $4=IDF 版本
    local out="$1" preset="$2" tcid="$3" idfv="$4"
    local sdkcfg="$out/sdkconfig.txt" lib f first=1

    # sel <标题> <符号前缀…>：把 sdkconfig 里以这些前缀开头的行原样列出，**包含**
    # `# CONFIG_X is not set`（"关掉了"和"打开了"一样是事实，不能只列 y 的那些）。
    sel() {
        local title="$1"; shift
        local args=() p
        for p in "$@"; do args+=(-e "^CONFIG_$p" -e "^# CONFIG_$p"); done
        echo "### $title"
        if grep -E "${args[@]}" "$sdkcfg" 2>/dev/null | sed 's/^/  /'; then :; else echo "  (无匹配)"; fi
        echo
    }

    {
        echo "# korvo2_oneye 固件组成清单（$(date -u +%Y-%m-%dT%H:%M:%SZ) 生成）"
        echo "#"
        echo "# 这份文件回答两个问题：**这版固件装了哪些模块功能**、**哪些开关真的生效了**。"
        echo "# 三个配套文件：sdkconfig.txt（生效开关全集）、preset-check.txt（逐项核对）、"
        echo "#              size-components.txt（按组件列体积 = 链接进固件的模块清单）"
        echo
        echo "## 0. 身份"
        echo "  preset            = $preset"
        echo "  toolchain_id      = $tcid"
        echo "  idf               = $idfv"
        echo "  board             = $(grep -m1 '^CONFIG_ESP32_S3_KORVO2_V3_BOARD=' "$sdkcfg" 2>/dev/null || echo '(未确认)')"
        echo "  flash_size        = $(grep -m1 '^CONFIG_ESPTOOLPY_FLASHSIZE=' "$sdkcfg" 2>/dev/null || echo '(未确认)')"
        echo "  cloud_endpoint    = $(grep -m1 '^CONFIG_ONEYE_FW_CLOUD_HOST=' "$sdkcfg" 2>/dev/null | cut -d= -f2-):$(grep -m1 '^CONFIG_ONEYE_FW_CLOUD_PORT=' "$sdkcfg" 2>/dev/null | cut -d= -f2-)"
        echo "  transport_tls     = $(grep -m1 '^CONFIG_ONEYE_FW_TRANSPORT_TLS=' "$sdkcfg" 2>/dev/null || echo '(未开)')"
        echo "  creds_required    = $(grep -m1 '^CONFIG_ONEYE_DEV_CREDS_REQUIRED=' "$sdkcfg" 2>/dev/null || echo '(未开启)')"
        echo
        echo "## 1. onEye 设备 SDK 模块（预编译归档 —— 真正被链接进去的就是这几个）"
        echo "  sdk_version       = $SDK_VERSION"
        echo "  sdk_src_sha256    = $(sdk_src_hash 2>/dev/null || echo '(未取到)')   # 与 CMake 哨兵同源：源码改过没重生成归档，这里会变"
        for lib in "${LIBS[@]}"; do
            f="$SDK_DIR/lib/$tcid/lib$lib.a"
            if [ -f "$f" ]; then
                printf '  %-22s %9s B  sha256=%s\n' "lib$lib.a" "$(stat -c%s "$f" 2>/dev/null || echo '?')" \
                    "$(sha256sum "$f" 2>/dev/null | cut -c1-16)"
            else
                printf '  %-22s %s\n' "lib$lib.a" "**(缺失 —— 该模块没进这份固件)**"
            fi
        done
        echo "  说明：base=核心/凭据与配置、mpp=物模型与属性、event=事件、log=日志、link=链路与重连、ble=蓝牙配网"
        echo
        echo "## 2. 关键功能开关（分组摘录；全集见 sdkconfig.txt）"
        echo
        sel "应用层（onEye）" ONEYE_
        sel "板卡与目标" IDF_TARGET ESP32_S3_KORVO2_V3_BOARD
        sel "无线（Wi-Fi / 蓝牙）" ESP_WIFI_ENABLED ESP_WIFI_SOFTAP_SUPPORT BT_ENABLED BT_BLE_ENABLED BT_NIMBLE_ENABLED BT_BLUEDROID_ENABLED
        sel "承载与加密（TLS / mbedTLS）" ONEYE_FW_TLS ESP_TLS_ MBEDTLS_CERTIFICATE_BUNDLE MBEDTLS_SSL_PROTO_TLS1_3
        sel "音频与语音（ADF / esp-sr）" AUDIO_BOARD ESP_SR_ MODEL_IN_FLASH AFE_ ADF_
        sel "存储与文件系统" FATFS_ SPIFFS_ SDMMC_ ESPTOOLPY_FLASHSIZE
        sel "分区与启动" PARTITION_TABLE_ BOOTLOADER_
        sel "内存（PSRAM / 内部保留）" SPIRAM
        sel "日志" LOG_DEFAULT_LEVEL LOG_MAXIMUM
        sel "协议栈与 RTOS" MQTT_ LWIP_DHCP LWIP_SNTP FREERTOS_HZ
        echo "## 3. 各组件体积"
        echo "  见同目录 size-components.txt（idf.py 原文，按组件列 flash/DRAM/IRAM 占用）"
    } > "$out/modules.txt" 2>/dev/null || warn "生成 modules.txt 失败"
}

build_firmware_esp() { # $1=idf 版本, $2=target —— Korvo-2 板级固件（仅 esp32s3；--firmware 时启用）
    local idfv="$1" target="$2" idf cc tcid out ex bdir preset defs certdir emptycerts
    [ "$WITH_FIRMWARE" = "1" ] || return 0
    if [ "$target" != "esp32s3" ]; then
        warn "固件轨仅支持 esp32s3（ESP32-S3-Korvo-2），跳过：$target"
        return 0
    fi
    idf="$IDF_ROOT/esp-idf-$idfv"; [ -d "$idf" ] || { warn "未找到 IDF：$idf，跳过固件轨"; return 0; }
    cc="$(esp_cc_for "$idf" "$target")"; [ -n "$cc" ] || return 0
    tcid="$(basename "$cc" | sed 's/-gcc$//')-gcc-$("$cc" -dumpversion)"
    ex="$SCRIPT_DIR/examples/oneye/korvo2_oneye"
    [ -d "$ex" ] || { warn "固件工程不存在：examples/oneye/korvo2_oneye"; return 2; }

    # ---- 档位决定"用哪些 defaults + 嵌不嵌证书"------------------------------------------------
    # production：叠加 sdkconfig.defaults.production，且 **不嵌证书**（`ONEYE_DEV_CREDS_REQUIRED=y`
    #   与内嵌证书同时成立时 CMakeLists 会直接 FATAL —— 那是"漏写分区的机器拿公用身份上线"的红线）。
    # bench：只叠加到 esp32s3（台面形态：SD 卡 Wi-Fi 文件、本地验证面都开着），并按需嵌 main/certs
    #   —— 台面一机一密自测要用它。
    preset="$FIRMWARE_PRESET"
    defs="sdkconfig.defaults;sdkconfig.defaults.esp32s3"
    certdir="$ex/main/certs"          # bench：目录不存在/不齐时 CMakeLists 自动视为"不嵌入"
    if [ "$preset" = "production" ]; then
        defs="$defs;sdkconfig.defaults.production"
        emptycerts="$OUT_ROOT/.build/empty-certs-$tcid"; mkdir -p "$emptycerts"
        certdir="$emptycerts"
    fi

    out="$OUT_ROOT/firmware/$tcid/korvo2_oneye-$preset"; mkdir -p "$out"
    bdir="$OUT_ROOT/.build/korvo2_oneye-$preset-$tcid"
    info "构建 Korvo-2 板级固件 korvo2_oneye（档位 **$preset**，$target @ IDF v$idfv）→ output/firmware/$tcid/korvo2_oneye-$preset/"
    (
        set +u
        . "$idf/export.sh" >/dev/null 2>&1
        export ADF_PATH="$SCRIPT_DIR"
        export CCACHE_ENABLE="${CCACHE_ENABLE:-0}"   # 并行构建时 ccache 竞争会 ICE
        cd "$ex" || exit 4
        rm -rf "$bdir" build                          # 残留 build 会让 set-target 静默回退
        # 独立 SDKCONFIG（不碰工程开发用 sdkconfig）+ 显式 defaults 链 + 显式证书目录
        idf.py -B "$bdir" -DSDKCONFIG="$bdir/sdkconfig" -DSDKCONFIG_DEFAULTS="$defs" \
               -DONEYE_FW_CERT_DIR="$certdir" \
               set-target "$target" > "$out/build.log" 2>&1 \
            && idf.py -B "$bdir" -DSDKCONFIG="$bdir/sdkconfig" -DSDKCONFIG_DEFAULTS="$defs" \
                      -DONEYE_FW_CERT_DIR="$certdir" build >> "$out/build.log" 2>&1
    ) || { warn "korvo2_oneye[$preset] 构建失败（见 firmware/$tcid/korvo2_oneye-$preset/build.log）"; return 2; }

    # ---- 出货自证：这份固件**到底装了什么**（量产要能查，而不是"应该叠了"）----------------------
    {
        echo "preset=$preset"
        echo "sdkconfig_defaults=$defs"
        echo "cert_dir=$certdir"
        echo "sdk_version=$SDK_VERSION"
        echo "toolchain_id=$tcid"
        echo "idf=$idfv"
        echo "built_at_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    } > "$out/preset.txt"
    cp -f "$bdir/sdkconfig" "$out/sdkconfig.txt" 2>/dev/null || true

    # ---- 模块/开关清单：一份给人看的 modules.txt，一份按组件列体积的 size-components.txt ---------
    # 为什么要这个：sdkconfig.txt 是 8 万字节的全集，"这台固件里到底有哪些模块功能"不该靠 grep 全集回答。
    # modules.txt 给**分组摘录 + SDK 模块与哈希**；size-components.txt 是 idf.py 的原文，按组件列
    # flash/IRAM/DRAM 占用 ⇒ "装了哪些模块、各占多大"有硬证据，也便于对比两次构建的体积漂移。
    firmware_modules_manifest "$out" "$preset" "$tcid" "$idfv"
    if (
        set +u
        . "$idf/export.sh" >/dev/null 2>&1
        export ADF_PATH="$SCRIPT_DIR"
        cd "$ex" || exit 4
        idf.py -B "$bdir" -DSDKCONFIG="$bdir/sdkconfig" -DSDKCONFIG_DEFAULTS="$defs" \
               -DONEYE_FW_CERT_DIR="$certdir" size-components > "$out/size-components.txt" 2>&1
    ); then
        ok "组件体积表已生成（size-components.txt：按组件列占用，= 装了哪些模块的硬证据）"
    else
        warn "size-components 未生成（不影响固件本身；modules.txt 仍在）"
    fi

    if [ -f "$ex/tools/check-sdkconfig-defaults.py" ]; then
        if [ "$preset" = "production" ]; then
            python3 "$ex/tools/check-sdkconfig-defaults.py" --sdkconfig "$out/sdkconfig.txt" \
                --require sdkconfig.defaults.production > "$out/preset-check.txt" 2>&1 \
                && ok "量产预设逐项核对通过（见 preset-check.txt）" \
                || { warn "量产预设核对**未通过**（见 firmware/$tcid/korvo2_oneye-$preset/preset-check.txt）"; }
        else
            python3 "$ex/tools/check-sdkconfig-defaults.py" --sdkconfig "$out/sdkconfig.txt" \
                > "$out/preset-check.txt" 2>&1 || true
        fi
    fi
    # 板卡选择核对（用**这份构建**的 sdkconfig，不再读工程目录那个 —— 工程 sdkconfig 已不参与构建）
    grep -E '^CONFIG_IDF_TARGET=|^CONFIG_IDF_TARGET_ESP32S3=|^CONFIG_ESP32_S3_KORVO2_V3_BOARD=' \
        "$out/sdkconfig.txt" > "$out/board-config.txt" 2>/dev/null || true
    cp -f "$bdir"/korvo2_oneye.bin "$bdir"/korvo2_oneye.elf "$bdir"/bootloader/bootloader.bin \
          "$bdir"/partition_table/partition-table.bin "$bdir"/srmodels/srmodels.bin "$out/" 2>/dev/null || true
    ( cd "$out" && sha256sum ./*.bin ./*.elf > SHA256SUMS 2>/dev/null ) || true
    if grep -q '^CONFIG_IDF_TARGET_ESP32S3=y' "$out/board-config.txt" 2>/dev/null \
       && grep -q '^CONFIG_ESP32_S3_KORVO2_V3_BOARD=y' "$out/board-config.txt" 2>/dev/null; then
        ok "korvo2_oneye[$preset] 构建通过（板卡选择已核对：esp32s3 + KORVO2_V3）"
    else
        warn "korvo2_oneye[$preset] 构建完成，但 board-config.txt 未确认 esp32s3 + KORVO2_V3（请人工核对）"
    fi
}

# ---------------------------------------------------------------- 主流程
info "oneye-dev-sdk 统一编译 v$SDK_VERSION：SDK=$SDK_DIR"
info "交付库：${LIBS[*]}（链接顺序 base → mpp → event → log；无聚合库）"
info "输出根：$OUT_ROOT"
mkdir -p "$OUT_ROOT/demo" "$OUT_ROOT/.build"
[ "$DO_CLEAN" = "1" ] && { info "清理输出"; rm -rf "$OUT_ROOT"/*; mkdir -p "$OUT_ROOT/demo" "$OUT_ROOT/.build"; }

if [ "$DEPS_LIST_ONLY" = "1" ]; then
    echo "SDK: $SDK_DIR (version $SDK_VERSION)"
    print_deps_list
    exit 0
fi

if [ -z "$TOOLCHAINS" ]; then
    TOOLCHAINS="host"
    if [ -n "$(list_idf_versions)" ]; then
        v="$(list_idf_versions | head -1)"
        TOOLCHAINS="host,esp32s3@$v"
    fi
fi
if [ "$TOOLCHAINS" = "all" ]; then
    TOOLCHAINS="host"
    for v in $(list_idf_versions); do
        TOOLCHAINS="$TOOLCHAINS,esp32s3@$v,esp32c3@$v"
    done
fi

LASTRC=0
GATE_RC=0
DEPS_RC=0
LAST_HOST_OUT=""; LAST_HOST_ID=""; ESP_OUTS=()
# 成败账本：每个工具链的 rc 都必须落到 BUILT / SKIPPED / FAILED 之一，不允许"默默当成功"。
# （原实现 `[ $rc -eq 1 ]` 只认 1：任何其它非零 rc，以及"规格写错"这支，都被静默丢弃。）
FAILED_TC=(); SKIPPED_TC=(); BUILT_TC=(); STALE_DIRS=()
IFS=',' read -ra SPECS <<< "$TOOLCHAINS"
for spec in "${SPECS[@]}"; do
    spec="$(echo "$spec" | xargs)"
    [ -n "$spec" ] || continue
    LAST_ERR=""; LAST_WARN=""
    case "$spec" in
        host) build_host; rc=$? ;;
        *@*)  build_esp "${spec#*@}" "${spec%@*}"; rc=$? ;;
        *)    warn "无法识别的工具链规格：$spec（用 host 或 <target>@<idf-version>）"; rc=3 ;;
    esac
    tc_id="${CUR_TCID:-$spec}"
    case "$rc" in
        0)  tc_commit; BUILT_TC+=("$tc_id") ;;
        2)  # **有意保留非致命**：所请求的工具链在本机不可用（缺 IDF / 交叉编译器）＝"跳过"，
            # 不是构建失败（`--toolchains all` 会枚举未安装的版本/芯片）。但必须在 stderr 与
            # 报告里显式可见，绝不伪装成成功。
            tc_commit
            SKIPPED_TC+=("$tc_id|${LAST_WARN:-未找到 IDF 或交叉编译器}") ;;
        *)  # rc=1（编译/归档/链接/清单/预编译发布失败）与 rc=3（规格无法识别）一律判定失败
            reason="${LAST_ERR:-工具链 $tc_id 构建失败（rc=$rc；无更详细的错误信息）}"
            tc_rollback "$tc_id" "$OUT_ROOT/$tc_id" "$reason"
            FAILED_TC+=("$tc_id|$reason")
            LASTRC=1 ;;
    esac
done

if [ -n "$LAST_HOST_OUT" ]; then
    run_gates "$LAST_HOST_OUT" || GATE_RC=1
    build_demo_host "$LAST_HOST_OUT"
    build_tests_host "$LAST_HOST_OUT"
fi
for entry in "${ESP_OUTS[@]:-}"; do
    [ -n "$entry" ] || continue
    IFS='|' read -r _tcid _t _v <<< "$entry"
    build_demo_esp "$_v" "$_t"
done
FIRMWARE_RC=0
if [ "$WITH_FIRMWARE" = "1" ]; then
    for entry in "${ESP_OUTS[@]:-}"; do
        [ -n "$entry" ] || continue
        IFS='|' read -r _tcid _t _v <<< "$entry"
        build_firmware_esp "$_v" "$_t" || FIRMWARE_RC=1
    done
fi

# ------------------------------------------------ 成败判定（**先判定，再写报告**）
# 报告与收尾横幅都必须反映真实结果：此前这里是"先打印完成、后判定失败"，于是失败运行照样以
# 「完成。产物树：」+ 一份无失败标记的 BUILD-REPORT.md 收尾，调用者/CI/人都会以为成功。
[ "$DEPS_FAIL" = "1" ] && { err "存在 required 依赖缺失（--deps-strict 生效），构建判定失败"; DEPS_RC=1; LASTRC=1; }
[ "$GATE_RC" = "1" ] && { err "门禁未通过（见上）"; LASTRC=1; }
[ "$FIRMWARE_RC" = "1" ] && { err "板级固件构建失败（见上）"; LASTRC=1; }
if [ "${#FAILED_TC[@]}" -gt 0 ]; then
    err "工具链构建失败摘要（${#FAILED_TC[@]} 个）："
    for entry in "${FAILED_TC[@]}"; do
        printf '%sERROR:   工具链 %s：%s%s\n' "$C_RED" "${entry%%|*}" "${entry#*|}" "$C_RESET" >&2
    done
    LASTRC=1
fi
if [ "$LASTRC" != "0" ]; then
    err "本次构建失败（含工具链/门禁/依赖/固件判定），退出码 1；详见 $OUT_ROOT/BUILD-REPORT.md"
fi

# ------------------------------------------------------------ 汇总报告
REPORT="$OUT_ROOT/BUILD-REPORT.md"
{
    echo "# oneye-dev-sdk 构建报告（v$SDK_VERSION，四领域库）"
    echo
    echo "- SDK 版本：\`$SDK_VERSION\`（源码：\`$SDK_DIR\`）"
    echo "- 交付库：\`liboneye_dev_base\` / \`liboneye_dev_mpp\` / \`liboneye_dev_event\` / \`liboneye_dev_log\`"
    echo "  （**无聚合库**；链接顺序 base → mpp → event → log）"
    echo "- 内嵌件：cJSON 1.7.19（MIT，符号前缀 \`$VENDOR_CJSON_PREFIX\`）"
    echo "- 时间：$(date -u +%Y-%m-%dT%H:%M:%SZ)（UTC）"
    echo "- 输出根：\`$OUT_ROOT\`"
    echo "- 承载：\`$TRANSPORT\`"
    echo
    if [ "$LASTRC" != "0" ]; then
        echo "> ⚠ **本次构建失败**（脚本退出码 1）—— 失败清单见文末「## 构建失败」。"
        [ "$DEPS_RC" = "1" ] && echo "> - 存在 required 依赖缺失（\`--deps-strict\` 生效）"
        [ "$GATE_RC" = "1" ] && echo "> - 门禁未通过（契约一致性 / 符号白名单，见「## 门禁与 demo」）"
        [ "$FIRMWARE_RC" = "1" ] && echo "> - 板级固件（\`--firmware\`）构建失败"
        if [ "${#FAILED_TC[@]}" -gt 0 ]; then
            echo "> - 工具链构建失败 ${#FAILED_TC[@]} 个：$(printf '%s ' "${FAILED_TC[@]%%|*}")"
            echo "> - 这些工具链的 \`toolchain.json\`/\`lib/*.a\` **未被本次刷新**（下方标 \`陈旧\` 的行来自上一次构建）"
        fi
        echo
    fi
    echo "## 工具链 × 库"
    echo
    echo "| 工具链目录 | 编译器 | 目标/IDF | 库产物 | 定义符号数 | 内嵌符号外泄 |"
    echo "| --- | --- | --- | --- | --- | --- |"
    for d in "$OUT_ROOT"/*/; do
        [ -f "$d/toolchain.json" ] || continue
        tcid="$(basename "$d")"
        row_mark=""
        is_stale "$tcid" && row_mark=" ⚠陈旧"
        ccv="$(sed -n 's/.*"compiler_version": "\(.*\)".*/\1/p' "$d/toolchain.json")"
        tgt="$(sed -n 's/.*"target": "\(.*\)".*/\1/p' "$d/toolchain.json")"
        idfv="$(sed -n 's/.*"idf_version": "\(.*\)".*/\1/p' "$d/toolchain.json")"
        libs="$(cd "$d/lib" 2>/dev/null && ls -1 *.a 2>/dev/null | tr '\n' ' ')"
        syms="$(nm -g --defined-only "$d"/lib/*.a 2>/dev/null | awk '{print $NF}' | grep -c '^oneye_' || true)"
        leak="$(nm -g --defined-only "$d"/lib/*.a 2>/dev/null | awk '{print $NF}' | grep -c '^cJSON_' || true)"
        echo "| \`$tcid\`$row_mark | $ccv | $tgt ${idfv:-（host）} | $libs | $syms | ${leak:-0} |"
    done
    if [ "${#STALE_DIRS[@]}" -gt 0 ]; then
        echo
        echo "> ⚠ 标 \`陈旧\` 的行：该工具链**本次构建失败**，行内数据来自上一次成功构建的产物（未被刷新），"
        echo "> **不代表本次结果**；对应目录已写 \`STALE.md\`（SDK 侧 \`lib/<tcid>/STALE.md\` 同）。"
    fi
    echo
    echo "## 依赖打包（声明见 components/oneye-dev-sdk/deps/）"
    echo
    echo "| 工具链目录 | 依赖（名字:状态:版本） |"
    echo "| --- | --- |"
    for d in "$OUT_ROOT"/*/; do
        [ -f "$d/deps.json" ] || continue
        tcid="$(basename "$d")"
        deps_txt="$(python3 - "$d/deps.json" <<'PY' 2>/dev/null || true
import json,sys
try:
    data=json.load(open(sys.argv[1]))
except Exception:
    print("（deps.json 解析失败）"); raise SystemExit
if not data:
    print("（无声明依赖）"); raise SystemExit
print("; ".join(f"{x.get('name')}:{x.get('status')}:{x.get('version') or '-'}" for x in data))
PY
)"
        [ -n "$deps_txt" ] || deps_txt="（解析需 python3）"
        echo "| \`$tcid\` | $deps_txt |"
    done
    echo
    echo "## 门禁与 demo"
    echo
    echo "| 工具链目录 | demo/测试产物 |"
    echo "| --- | --- |"
    for d in "$OUT_ROOT"/demo/*/; do
        [ -d "$d" ] || continue
        files="$(cd "$d" && ls -1 | tr '\n' ' ')"
        echo "| \`$(basename "$d")\` | $files |"
    done
    echo
    echo "## 板级固件（--firmware）"
    echo
    if [ "$WITH_FIRMWARE" = "1" ]; then
        echo "| 工具链目录 | 产物 | 板卡选择（sdkconfig） |"
        echo "| --- | --- | --- |"
        for d in "$OUT_ROOT"/firmware/*/korvo2_oneye/; do
            [ -d "$d" ] || continue
            tcid="$(basename "$(dirname "$d")")"
            files="$(cd "$d" && ls -1 | tr '\n' ' ')"
            board="$(tr '\n' ' ' < "$d/board-config.txt" 2>/dev/null || true)"
            echo "| \`$tcid\` | $files | ${board:-（未记录）} |"
        done
    else
        echo "（未启用：加 \`--firmware\` 并指定 \`esp32s3@5.5.5\` 可构建 \`examples/oneye/korvo2_oneye\`）"
    fi
    echo
    echo "## 契约对账"
    echo
    echo "- 契约一致性：\`tools/check-topics.py\`（SDK topic 字面量 ↔ \`backend/contracts/api/mqtt/asyncapi.yaml\`）"
    echo "- 后端侧同源对账：\`cd backend && python3 scripts/contract_check.py\`"
    echo "- 设备面规范：\`backend/contracts/api/mqtt/传输规范.md\`"
    if [ "${#FAILED_TC[@]}" -gt 0 ]; then
        echo
        echo "## 构建失败（本次运行）"
        echo
        echo "| 工具链 | 失败原因 | 本次产物 |"
        echo "| --- | --- | --- |"
        for entry in "${FAILED_TC[@]}"; do
            tcid="${entry%%|*}"
            reason="${entry#*|}"
            reason="${reason//|/\\|}"     # 表格转义（原因里出现竖线时不破坏表格）
            if is_stale "$tcid"; then
                what="已回滚为上一次产物并标注 \`STALE.md\`（陈旧）"
            else
                what="无（未产出新产物；半成品隔离在 \`output/.build/failed-$tcid/\`）"
            fi
            echo "| \`$tcid\` | $reason | $what |"
        done
        echo
        echo "**本次构建失败**：以上工具链的 \`toolchain.json\`/\`lib/*.a\` 未被本次刷新，退出码非 0。"
        echo "请勿把标 \`陈旧\` 的产物或 SDK 侧 \`lib/<工具链>/\` 的旧归档当作本次交付。"
    fi
    if [ "${#SKIPPED_TC[@]}" -gt 0 ]; then
        echo
        echo "## 已请求但未构建（跳过，不计入失败）"
        echo
        echo "| 工具链 | 原因 |"
        echo "| --- | --- |"
        for entry in "${SKIPPED_TC[@]}"; do
            sk_reason="${entry#*|}"
            echo "| \`${entry%%|*}\` | ${sk_reason//|/\\|} |"
        done
        echo
        echo "（本机缺 IDF 或交叉编译器；这些工具链**没有**产物，也不代表成功。）"
    fi
} > "$REPORT"

if [ "$LASTRC" = "0" ]; then
    info "完成。产物树："
else
    info "构建失败。产物树（⚠ 带 STALE.md 的目录是上一次的陈旧产物）："
fi
( cd "$OUT_ROOT" && find . -maxdepth 2 -mindepth 1 \( -name '.build' -prune -o -print \) | sort | sed 's/^/    /' )
info "汇总报告：$REPORT"
if [ "$LASTRC" != "0" ]; then
    err "构建失败：见上方 ERROR 摘要与 $REPORT（退出码 1）"
elif [ "${#SKIPPED_TC[@]}" -gt 0 ]; then
    warn "已请求但未构建（跳过，不计入失败）：$(printf '%s ' "${SKIPPED_TC[@]%%|*}")"
fi
exit $LASTRC
