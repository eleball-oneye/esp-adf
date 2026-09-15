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
TRANSPORT="mqtt-tcp,mqtt-tls,mqtt-ws,mqtt-wss"

VENDOR_CJSON_PREFIX="oev_cjson_"
LIBS=(oneye_dev_base oneye_dev_mpp oneye_dev_event oneye_dev_log)

C_RESET=$'\033[0m'; C_BOLD=$'\033[1m'; C_GREEN=$'\033[32m'; C_YELLOW=$'\033[33m'; C_RED=$'\033[31m'
info()  { printf '%s==> %s%s\n' "$C_BOLD" "$*" "$C_RESET"; }
ok()    { printf '    %s%s%s\n' "$C_GREEN" "$*" "$C_RESET"; }
warn()  { printf '    %s%s%s\n' "$C_YELLOW" "$*" "$C_RESET"; }
err()   { printf '%sERROR: %s%s\n' "$C_RED" "$*" "$C_RESET" >&2; }

usage() { sed -n '2,60p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

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
        --no-deps)       DEPS_MODE="none"; shift ;;
        --clean)         DO_CLEAN=1; shift ;;
        --list)          DO_LIST=1; shift ;;
        -h|--help)       usage; exit 0 ;;
        *) err "未知参数：$1"; usage; exit 2 ;;
    esac
done

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
    "$SRC_INTERNAL/oneye_internal.c"
    "$SRC_INTERNAL/oneye_test.c"
    "$SRC_INTERNAL/oneye_osal_posix.c"
    "$SRC_INTERNAL/oneye_osal_idf.c"
    "$SRC_INTERNAL/oneye_osal_tls_mbedtls.c"
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
VENDOR_SRCS=( "$SDK_DIR/vendor/cjson/cJSON.c" )

lib_srcs() {
    local out=""
    case "$1" in
        oneye_dev_base)  out="$(printf '%s\n' "${BASE_SRCS[@]}")" ;;
        oneye_dev_mpp)   out="$(printf '%s\n' "${MPP_SRCS[@]}")" ;;
        oneye_dev_event) out="$(printf '%s\n' "${EVENT_SRCS[@]}")" ;;
        oneye_dev_log)   out="$(printf '%s\n' "${LOG_SRCS[@]}")" ;;
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
    echo "SDK: $SDK_DIR (version $SDK_VERSION, 4 库：${LIBS[*]})"
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
        || { err "内嵌件编译失败"; return 1; }
    vendor_object_private "$scratch/oneye_vendor_raw.o" "$vendor_priv_o" "$nm_bin" "$objcopy_bin" || return 1

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
            [ -f "$s" ] || { err "源文件缺失：$s"; return 1; }
            o="$scratch/$(basename "${s%.c}").o"
            "$c" $_cflags -I"$SDK_DIR/include" -I"$SRC_INTERNAL" -I"$SDK_DIR/vendor/cjson" \
                 -c "$s" -o "$o" || { err "编译失败：$s"; return 1; }
            objs+=("$o")
        done < <(lib_srcs "$lib")
        ar rcs "$scratch/lib${lib}_body.a" "${objs[@]}" || return 1
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
    } | ar -M 2>&1)" || { err "合并内嵌件失败（ar -M）："; printf '%s\n' "$ar_out" | sed 's/^/    /'; return 1; }
    if [ ! -f "$out/lib/liboneye_dev_base.a" ]; then
        err "合并内嵌件后未生成 liboneye_dev_base.a："; printf '%s\n' "$ar_out" | sed 's/^/    /'; return 1
    fi
    ok "lib/liboneye_dev_base.a（含内嵌 cJSON，符号已隔离） $(stat -c%s "$out/lib/liboneye_dev_base.a") B"

    for lib in oneye_dev_mpp oneye_dev_event oneye_dev_log; do
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
        || { err "链接 liboneye_dev_base.so 失败"; return 1; }
    for lib in oneye_dev_mpp oneye_dev_event oneye_dev_log; do
        "$c" -shared -Wl,-soname,lib${lib}.so.0 -Wl,--version-script="$vermap" \
            -o "$out/lib/lib${lib}.so.$SDK_VERSION_NUM" \
            ${LIB_OBJS[$lib]} -L"$out/lib" -loneye_dev_base -Wl,-rpath,'$ORIGIN' \
            || { err "链接 lib${lib}.so 失败"; return 1; }
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
Libs: -L\${libdir} -Wl,--start-group -loneye_dev_mpp -loneye_dev_event -loneye_dev_log -loneye_dev_base -Wl,--end-group
Libs.private: $dep_libs
Cflags: -I\${includedir}
EOF
    ok "oneye-dev-sdk.pc（pkg-config 一次展开 4 库）"

    bundle_deps_host "$out"
    write_manifest "$out" "$tcid" "$c" "$(cc_full_version "$c")" "x86_64/host" "" "$cflags"
    [ "$PUBLISH_REPO_LIB" = "1" ] && publish_to_repo_lib "$out" "$tcid"
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
IDF_PROBE_REQUIRES="esp_netif mbedtls esp_event nvs_flash esp_timer"

idf_probe_flags() { # $1=idf 路径 $2=target $3=tcid → 回显 flags 文件路径；失败返回 1
    local idf="$1" target="$2" tcid="$3"
    local cache="$OUT_ROOT/.build/idfflags-$tcid.txt"
    local meta="$cache.meta"
    local probe="$OUT_ROOT/.build/idfprobe-$tcid"
    local want="idf=$idf target=$target"

    # 缓存有效判据：flags 在、stamp 与本次一致、且 sdkconfig.h 仍在（探针目录未被清理）。
    if [ -s "$cache" ] && [ -f "$meta" ] && [ "$(cat "$meta" 2>/dev/null)" = "$want" ] \
       && [ -f "$probe/build/config/sdkconfig.h" ]; then
        echo "$cache"; return 0
    fi

    mkdir -p "$probe/main"
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
    vendor_object_private "$scratch/oneye_vendor_raw.o" "$vendor_priv_o" "$nm_bin" "$objcopy_bin" || return 1

    local lib s objs=() o
    declare -A LIB_BODY=()
    for lib in "${LIBS[@]}"; do
        objs=()
        while IFS= read -r s; do
            [ -n "$s" ] || continue
            [ -f "$s" ] || { err "源文件缺失：$s"; return 1; }
            o="$scratch/$(basename "${s%.c}").o"
            # 注意顺序：我们自己的头目录放在 IDF 的 include 之前，避免同名头被 IDF 抢命中
            # （vendor/cjson 的 cJSON.h 与 src/internal/cJSON.h 别名壳必须优先）。
            "$cc" "${IDF_FLAGS[@]}" $own_flags -I"$SDK_DIR/include" -I"$SRC_INTERNAL" -I"$SDK_DIR/vendor/cjson" \
                  -c "$s" -o "$o" || { err "编译失败：$s（$tcid）"; return 1; }
            objs+=("$o")
        done < <(lib_srcs "$lib")
        "$ar_bin" rcs "$scratch/lib${lib}_body.a" "${objs[@]}" || return 1
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
    for lib in oneye_dev_mpp oneye_dev_event oneye_dev_log; do
        cp -f "${LIB_BODY[$lib]}" "$out/lib/lib${lib}.a"
        ok "lib/lib${lib}.a $(stat -c%s "$out/lib/lib${lib}.a") B"
    done

    bundle_deps_esp "$out" "$idf" "$idfv"
    local n_inc n_def idf_std cflags_summary
    n_inc="$(grep -c '^-I' "$idf_flags_file" 2>/dev/null || true)"
    n_def="$(grep -cE '^-[DU]' "$idf_flags_file" 2>/dev/null || true)"
    idf_std="$(grep -m1 '^-std=' "$idf_flags_file" 2>/dev/null || true)"
    cflags_summary="IDF 生成参数 ${idf_std:-（无 -std=）} -I×${n_inc} -D/-U×${n_def}（全文见 idf-cflags.txt） $own_flags"
    write_manifest "$out" "$tcid" "$cc" "$("$cc" -dumpfullversion 2>/dev/null || "$cc" -dumpversion)" "$target" "v$idfv" "$cflags_summary"
    [ "$PUBLISH_REPO_LIB" = "1" ] && publish_to_repo_lib "$out" "$tcid"
    ESP_OUTS+=("$tcid|$target|$idfv")
    return 0
}

publish_to_repo_lib() {
    local out="$1" tcid="$2"
    mkdir -p "$SDK_DIR/lib/$tcid"
    cp -f "$out/lib/"*.a "$SDK_DIR/lib/$tcid/" 2>/dev/null || true
    cp -f "$out/lib/"*.so* "$SDK_DIR/lib/$tcid/" 2>/dev/null || true
    cp -f "$out/toolchain.json" "$SDK_DIR/lib/$tcid/" 2>/dev/null || true
    cp -f "$out/SHA256SUMS" "$SDK_DIR/lib/$tcid/" 2>/dev/null || true
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
  "built_at_utc": "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
}
EOF
    ( cd "$out" && find lib include -type f 2>/dev/null | sort | xargs -r sha256sum > SHA256SUMS
      [ -f oneye-dev-sdk.pc ] && sha256sum oneye-dev-sdk.pc >> SHA256SUMS )
}

# ======================================================= 门禁与 demo
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
    local libs="-Wl,--start-group $out/lib/liboneye_dev_mpp.a $out/lib/liboneye_dev_event.a $out/lib/liboneye_dev_log.a $out/lib/liboneye_dev_base.a -Wl,--end-group"
    local deplibs; deplibs="$(pkg-config --libs mbedtls mbedx509 mbedcrypto 2>/dev/null || echo '-lmbedtls -lmbedx509 -lmbedcrypto')"
    local name
    for name in base event log mpp all; do
        "$c" $cflags -I"$out/include" "$SDK_DIR/examples/linux/demo_${name}.c" $libs $deplibs -lpthread -lm \
            -o "$demo/demo_${name}" || { err "demo_${name} 编译失败"; continue; }
    done
    "$c" $cflags -I"$out/include" "$SDK_DIR/examples/linux/demo_all.c" \
        -L"$out/lib" -Wl,--start-group -loneye_dev_mpp -loneye_dev_event -loneye_dev_log -loneye_dev_base -Wl,--end-group \
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
    local libs="-Wl,--start-group $out/lib/liboneye_dev_mpp.a $out/lib/liboneye_dev_event.a $out/lib/liboneye_dev_log.a $out/lib/liboneye_dev_base.a -Wl,--end-group"
    local deplibs; deplibs="$(pkg-config --libs mbedtls mbedx509 mbedcrypto 2>/dev/null || echo '-lmbedtls -lmbedx509 -lmbedcrypto')"
    info "构建并运行宿主单测（tests/）"
    if "$c" -O1 -g -Wall -Wextra -std=c99 -I"$out/include" -I"$SRC_INTERNAL" -I"$SDK_DIR/tests" \
        -I"$SDK_DIR/vendor/cjson" "${srcs[@]}" $libs $deplibs -lpthread -lm -o "$demo/oneye_dev_tests" 2>"$demo/oneye_dev_tests.build.log"; then
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

build_firmware_esp() { # $1=idf 版本, $2=target —— Korvo-2 板级固件（仅 esp32s3；--firmware 时启用）
    local idfv="$1" target="$2" idf cc tcid out ex bdir
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
    out="$OUT_ROOT/firmware/$tcid/korvo2_oneye"; mkdir -p "$out"
    bdir="$OUT_ROOT/.build/korvo2_oneye-$tcid"
    info "构建 Korvo-2 板级固件 korvo2_oneye（$target @ IDF v$idfv）→ output/firmware/$tcid/korvo2_oneye/"
    (
        set +u
        . "$idf/export.sh" >/dev/null 2>&1
        export ADF_PATH="$SCRIPT_DIR"
        export CCACHE_ENABLE="${CCACHE_ENABLE:-0}"   # 并行构建时 ccache 竞争会 ICE
        cd "$ex" || exit 4
        rm -rf "$bdir" build                          # 残留 build 会让 set-target 静默回退
        idf.py -B "$bdir" set-target "$target" > "$out/build.log" 2>&1 \
            && idf.py -B "$bdir" build >> "$out/build.log" 2>&1
    ) || { warn "korvo2_oneye 构建失败（见 firmware/$tcid/korvo2_oneye/build.log）"; return 2; }

    # 板卡选择核对（sdkconfig 落在**工程目录**，不在构建目录）
    grep -E '^CONFIG_IDF_TARGET=|^CONFIG_IDF_TARGET_ESP32S3=|^CONFIG_ESP32_S3_KORVO2_V3_BOARD=' \
        "$ex/sdkconfig" > "$out/board-config.txt" 2>/dev/null || true
    cp -f "$bdir"/korvo2_oneye.bin "$bdir"/korvo2_oneye.elf "$bdir"/bootloader/bootloader.bin \
          "$bdir"/partition_table/partition-table.bin "$out/" 2>/dev/null || true
    ( cd "$out" && sha256sum ./*.bin ./*.elf > SHA256SUMS 2>/dev/null ) || true
    if grep -q '^CONFIG_IDF_TARGET_ESP32S3=y' "$out/board-config.txt" 2>/dev/null \
       && grep -q '^CONFIG_ESP32_S3_KORVO2_V3_BOARD=y' "$out/board-config.txt" 2>/dev/null; then
        ok "korvo2_oneye 构建通过（板卡选择已核对：esp32s3 + KORVO2_V3）"
    else
        warn "korvo2_oneye 构建完成，但 board-config.txt 未确认 esp32s3 + KORVO2_V3（请人工核对）"
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
LAST_HOST_OUT=""; LAST_HOST_ID=""; ESP_OUTS=()
IFS=',' read -ra SPECS <<< "$TOOLCHAINS"
for spec in "${SPECS[@]}"; do
    spec="$(echo "$spec" | xargs)"
    [ -n "$spec" ] || continue
    case "$spec" in
        host) build_host; rc=$?; [ $rc -ne 0 ] && LASTRC=$rc ;;
        *@*)  build_esp "${spec#*@}" "${spec%@*}"; rc=$?; [ $rc -eq 1 ] && LASTRC=1 ;;
        *) warn "无法识别的工具链规格：$spec（用 host 或 <target>@<idf-version>）" ;;
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
    echo "## 工具链 × 库"
    echo
    echo "| 工具链目录 | 编译器 | 目标/IDF | 库产物 | 定义符号数 | 内嵌符号外泄 |"
    echo "| --- | --- | --- | --- | --- | --- |"
    for d in "$OUT_ROOT"/*/; do
        [ -f "$d/toolchain.json" ] || continue
        tcid="$(basename "$d")"
        ccv="$(sed -n 's/.*"compiler_version": "\(.*\)".*/\1/p' "$d/toolchain.json")"
        tgt="$(sed -n 's/.*"target": "\(.*\)".*/\1/p' "$d/toolchain.json")"
        idfv="$(sed -n 's/.*"idf_version": "\(.*\)".*/\1/p' "$d/toolchain.json")"
        libs="$(cd "$d/lib" 2>/dev/null && ls -1 *.a 2>/dev/null | tr '\n' ' ')"
        syms="$(nm -g --defined-only "$d"/lib/*.a 2>/dev/null | awk '{print $NF}' | grep -c '^oneye_' || true)"
        leak="$(nm -g --defined-only "$d"/lib/*.a 2>/dev/null | awk '{print $NF}' | grep -c '^cJSON_' || true)"
        echo "| \`$tcid\` | $ccv | $tgt ${idfv:-（host）} | $libs | $syms | ${leak:-0} |"
    done
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
} > "$REPORT"

info "完成。产物树："
( cd "$OUT_ROOT" && find . -maxdepth 2 -mindepth 1 \( -name '.build' -prune -o -print \) | sort | sed 's/^/    /' )
info "汇总报告：$REPORT"
[ "$DEPS_FAIL" = "1" ] && { err "存在 required 依赖缺失（--deps-strict 生效），构建判定失败"; LASTRC=1; }
[ "$GATE_RC" = "1" ] && { err "门禁未通过（见上）"; LASTRC=1; }
[ "$FIRMWARE_RC" = "1" ] && { err "板级固件构建失败（见上）"; LASTRC=1; }
exit $LASTRC
