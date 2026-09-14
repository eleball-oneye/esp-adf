#!/usr/bin/env bash
# =============================================================================
# oneye-dev-sdk 统一编译脚本（位于 esp-adf 仓库根目录）
#
# 一次命令、可选工具链，产出「头文件 + 库（.a/.so）+ 接入 demo」，全部落到
# **esp-adf 仓库内的 output/**（已在 .gitignore 中忽略，不入库），按工具链名称分目录：
#
#   output/
#   ├── <toolchain-id>/                 # 例：x86_64-linux-gnu-gcc-13.3.0
#   │   ├── include/oneye_dev_sdk.h         头文件（随库交付）
#   │   ├── include/oneye_dev_sdk_test.h
#   │   ├── include/<依赖名>/…              **依赖的第三方头文件**（宿主轨随包复制）
#   │   ├── lib/liboneye_dev_sdk.a          静态库
#   │   ├── lib/liboneye_dev_sdk.so*        动态库（仅宿主）
#   │   ├── lib/lib<依赖>.a|.so*            **依赖的库**（宿主轨随包复制）
#   │   ├── toolchain.json                  构建口径（编译器/版本/IDF 版本/编译选项/依赖摘要）
#   │   ├── deps.json                       依赖清单（名字/方式/版本/状态：bundled|provided-by-idf|missing）
#   │   └── SHA256SUMS                      校验值（含随包复制的依赖头文件与库）
#   ├── demo/
#   │   └── <toolchain-id>/             # demo 产物与运行日志（与库分离）
#   │       ├── demo_static / demo_shared / demo_dlopen(.log)
#   │       └── esp-hello_oneye/hello_oneye.bin（含 build.log）
#   └── BUILD-REPORT.md                 本次构建汇总（工具链 × 产物 × 校验值 × 依赖）
#
# 依赖打包：声明见 components/oneye-dev-sdk/deps/（linux.conf 用 pkg-config 解析并**复制头文件+库**；
# idf.conf 为 IDF 组件，**登记**组件名/版本、库由 IDF 提供，可用 --vendor-idf-headers 复制其头文件）。
#
# 工具链标识规则：<compiler-triple>-gcc-<compiler-version>
#   host                -> x86_64-linux-gnu-gcc-13.3.0
#   esp32s3@5.5.5       -> xtensa-esp32s3-elf-gcc-14.2.0
#   esp32s3@6.0.3       -> xtensa-esp32s3-elf-gcc-15.2.0
#   esp32c3@5.5.5       -> riscv32-esp-elf-gcc-14.2.0
#
# 用法示例（在 esp-adf 根目录执行）：
#   ./build-all.sh --list
#   ./build-all.sh --toolchains host
#   ./build-all.sh --toolchains esp32s3@5.5.5,esp32c3@5.5.5 --no-demo
#   ./build-all.sh --toolchains all                    # host + 每套已装 IDF 的 esp32s3/esp32c3
#   OUT=/tmp/out ./build-all.sh --toolchains host       # 或 --out <dir> 覆盖输出目录
#   ./build-all.sh --deps-list                          # 查看依赖声明
#   ./build-all.sh --deps-strict --toolchains host      # required 依赖缺失即判定失败
#   ./build-all.sh --vendor-idf-headers --toolchains esp32s3@5.5.5   # 连 IDF 组件头文件一并复制
#   ./build-all.sh --no-deps --toolchains host          # 不做依赖打包
# =============================================================================

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

OUT_ROOT=""
SDK_DIR=""
IDF_ROOT="${IDF_ROOT:-$HOME/esp}"
TOOLCHAINS=""
WITH_DEMO=1
DO_CLEAN=0
DO_LIST=0
PUBLISH_REPO_LIB=1
DEPS_MODE="auto"        # auto | none
DEPS_STRICT=0           # 1 = required 依赖缺失即失败
VENDOR_IDF_HEADERS=0    # 1 = 复制 IDF 组件头文件到产物 include/idf-deps/
DEPS_LIST_ONLY=0

C_RESET=$'\033[0m'; C_BOLD=$'\033[1m'; C_GREEN=$'\033[32m'; C_YELLOW=$'\033[33m'; C_RED=$'\033[31m'
info()  { printf '%s==> %s%s\n' "$C_BOLD" "$*" "$C_RESET"; }
ok()    { printf '    %s%s%s\n' "$C_GREEN" "$*" "$C_RESET"; }
warn()  { printf '    %s%s%s\n' "$C_YELLOW" "$*" "$C_RESET"; }
err()   { printf '%sERROR: %s%s\n' "$C_RED" "$*" "$C_RESET" >&2; }

usage() {
    sed -n '2,40p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

# ---------------------------------------------------------------- 参数解析
while [ $# -gt 0 ]; do
    case "$1" in
        -t|--toolchains) TOOLCHAINS="${2:-}"; shift 2 ;;
        --sdk)           SDK_DIR="${2:-}"; shift 2 ;;
        -o|--out)        OUT_ROOT="${2:-}"; shift 2 ;;
        --idf-root)      IDF_ROOT="${2:-}"; shift 2 ;;
        --no-demo)       WITH_DEMO=0; shift ;;
        --no-publish)    PUBLISH_REPO_LIB=0; shift ;;
        --deps)          DEPS_MODE="${2:-auto}"; shift 2 ;;
        --deps-strict)   DEPS_STRICT=1; shift ;;
        --deps-list)     DEPS_LIST_ONLY=1; shift ;;
        --vendor-idf-headers) VENDOR_IDF_HEADERS=1; shift ;;
        --no-deps)       DEPS_MODE="none"; shift ;;
        --clean)         DO_CLEAN=1; shift ;;
        --list)          DO_LIST=1; shift ;;
        -h|--help)       usage; exit 0 ;;
        *) err "未知参数：$1"; usage; exit 2 ;;
    esac
done

# ------------------------------------------------- SDK 与输出目录自动探测
# 本脚本规范位置：esp-adf 仓库根目录（集成仓）。SDK 组件位于 components/oneye-dev-sdk。
if [ -z "$SDK_DIR" ]; then
    if [ -f "$SCRIPT_DIR/include/oneye_dev_sdk.h" ]; then
        SDK_DIR="$SCRIPT_DIR"                                          # 脚本位于 SDK 仓根
    elif [ -f "$SCRIPT_DIR/components/oneye-dev-sdk/include/oneye_dev_sdk.h" ]; then
        SDK_DIR="$SCRIPT_DIR/components/oneye-dev-sdk"                 # 脚本位于 esp-adf 根（推荐）
    elif [ -f "$SCRIPT_DIR/../include/oneye_dev_sdk.h" ]; then
        SDK_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"                        # 脚本位于 <sdk>/tools/
    elif [ -f "$PWD/esp-adf/components/oneye-dev-sdk/include/oneye_dev_sdk.h" ]; then
        SDK_DIR="$PWD/esp-adf/components/oneye-dev-sdk"
    else
        err "未找到 oneye-dev-sdk，请用 --sdk <path> 指定"; exit 2
    fi
fi
[ -f "$SDK_DIR/include/oneye_dev_sdk.h" ] || { err "SDK 目录无效：$SDK_DIR"; exit 2; }
[ -d "$SDK_DIR/src" ] || { err "SDK 缺少 src/（参考实现）"; exit 2; }

# 输出根：默认放在 esp-adf 根目录下的 output/（已 gitignore）；脚本在别处运行时用当前目录
if [ -z "$OUT_ROOT" ]; then
    if [ -f "$SCRIPT_DIR/components/oneye-dev-sdk/include/oneye_dev_sdk.h" ]; then
        OUT_ROOT="$SCRIPT_DIR/output"
    else
        OUT_ROOT="$PWD/output"
    fi
fi
SDK_VERSION="$(sed -n 's/^#define ONEYE_DEV_SDK_VERSION_STR[[:space:]]*"\(.*\)"/\1/p' "$SDK_DIR/include/oneye_dev_sdk.h" | head -1)"
SDK_VERSION="${SDK_VERSION:-unknown}"
# 数值版本（用于库文件名/SONAME，不含 -ref 之类的后缀）
V_MAJOR="$(sed -n 's/^#define ONEYE_DEV_SDK_VERSION_MAJOR[[:space:]]*\([0-9]*\).*/\1/p' "$SDK_DIR/include/oneye_dev_sdk.h" | head -1)"
V_MINOR="$(sed -n 's/^#define ONEYE_DEV_SDK_VERSION_MINOR[[:space:]]*\([0-9]*\).*/\1/p' "$SDK_DIR/include/oneye_dev_sdk.h" | head -1)"
V_PATCH="$(sed -n 's/^#define ONEYE_DEV_SDK_VERSION_PATCH[[:space:]]*\([0-9]*\).*/\1/p' "$SDK_DIR/include/oneye_dev_sdk.h" | head -1)"
SDK_VERSION_NUM="${V_MAJOR:-0}.${V_MINOR:-0}.${V_PATCH:-0}"

# ------------------------------------------------------------ 工具链发现
list_idf_versions() {
    local d
    for d in "$IDF_ROOT"/esp-idf-*; do
        [ -d "$d" ] || continue
        basename "$d" | sed 's/^esp-idf-//'
    done
}

if [ "$DO_LIST" = "1" ]; then
    echo "SDK: $SDK_DIR (version $SDK_VERSION)"
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

# 交叉编译器：从该 IDF 版本自己的工具索引解析（避免 PATH 混入其它 IDF 版本）
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

# gcc>=7 的 -dumpversion 只给主版本号（"13"），库目录名需要完整版本（"13.3.0"）
cc_full_version() { local c="$1" v; v="$("$c" -dumpfullversion 2>/dev/null || true)"; [ -n "$v" ] || v="$("$c" -dumpversion)"; echo "$v"; }

# ======================================================= 依赖解析与打包
# 目标：把 liboneye_dev_sdk 的第三方依赖（头文件 + 库）一并打进产物目录，
#       使 output/<工具链>/ 成为自包含交付单元。声明见 components/oneye-dev-sdk/deps/。
DEPS_FAIL=0

deps_conf_for() { # $1=host|esp
    case "$1" in
        host) echo "$SDK_DIR/deps/linux.conf" ;;
        esp)  echo "$SDK_DIR/deps/idf.conf" ;;
    esac
}

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

# pkg-config 依赖：复制头文件到 include/<name>/、库到 lib/，返回 JSON 条目
copy_dep_pkgconfig() { # $1=out $2=name $3=pkgname $4=required $5=desc
    local out="$1" name="$2" ident="$3" req="$4" desc="$5"
    local ver incd libd libname f
    local -a inc_dirs=() lib_dirs=() lib_names=() hdrs=() libs_copied=()
    ver="$(pkg-config --modversion "$ident")"

    # 头文件候选目录：-I 参数 + pkg-config 的 includedir 变量
    # （很多系统包的头文件在默认搜索路径 /usr/include 下，不带 -I，必须靠 includedir 兜底）
    for incd in $(pkg-config --cflags-only-I "$ident"); do inc_dirs+=("${incd#-I}"); done
    incd="$(pkg-config --variable=includedir "$ident" 2>/dev/null || true)"; [ -n "$incd" ] && inc_dirs+=("$incd")
    for incd in $(printf '%s\n' "${inc_dirs[@]:-}" | awk 'NF && !seen[$0]++'); do
        [ -d "$incd" ] || continue
        if [ -d "$incd/$name" ]; then                        # 例：/usr/include/cjson、/usr/include/mbedtls
            mkdir -p "$out/include"
            cp -a "$incd/$name" "$out/include/" 2>/dev/null && hdrs+=("include/$name")
        elif [ "$incd" = "/usr/include" ] || [ "$incd" = "/usr/local/include" ]; then
            warn "  $name：头文件位于系统根 $incd 且无同名子目录（未整体复制）"
        else
            mkdir -p "$out/include/$name"
            cp -a "$incd/." "$out/include/$name/" 2>/dev/null && hdrs+=("include/$name")
        fi
    done

    # 库候选目录：-L 参数 + pkg-config 的 libdir 变量 + 编译器默认库目录
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
    ok "依赖 $name v$ver：头文件 → include/$name/（${#hdrs[@]} 项）、库 → lib/（${#libs_copied[@]} 个文件）"
    printf '{"name":"%s","kind":"pkgconfig","pkgconfig":"%s","version":"%s","required":"%s","status":"bundled","desc":"%s"}' \
        "$name" "$ident" "$ver" "$req" "$desc"
}

# 宿主轨依赖打包
bundle_deps_host() { # $1=out dir
    local out="$1" conf; conf="$(deps_conf_for host)"
    mkdir -p "$out/include" "$out/lib"
    if [ "$DEPS_MODE" = "none" ]; then printf '[]\n' > "$out/deps.json"; return 0; fi
    if [ ! -f "$conf" ]; then warn "未找到依赖声明 $conf（跳过依赖打包）"; printf '[]\n' > "$out/deps.json"; return 0; fi
    if ! command -v pkg-config >/dev/null 2>&1; then
        warn "缺少 pkg-config，无法解析依赖（安装：sudo apt-get install -y pkg-config）；deps.json 标记为 missing"
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
                warn "依赖缺失（可选）：$name（$ident）——安装后重跑即可随包复制"
            fi
        fi
        if [ "$first" = "1" ]; then json="$json$entry"; first=0; else json="$json,$entry"; fi
    done < <(grep -v '^[[:space:]]*#' "$conf" | grep -v '^[[:space:]]*$')
    printf '%s]\n' "$json" > "$out/deps.json"
}

# ESP-IDF 轨依赖登记（库由 IDF 提供，不复制；可选复制头文件）
bundle_deps_esp() { # $1=out dir, $2=idf path, $3=idf version
    local out="$1" idf="$2" idfv="${3:-}" conf; conf="$(deps_conf_for esp)"
    if [ "$DEPS_MODE" = "none" ] || [ ! -f "$conf" ]; then printf '[]\n' > "$out/deps.json"; return 0; fi
    local json="[" first=1 name kind req desc cdir ver status entry alt
    while IFS='|' read -r name kind req desc; do
        name="$(echo "${name:-}" | xargs)"; kind="$(echo "${kind:-}" | xargs)"
        req="$(echo "${req:-}" | xargs)"; desc="$(echo "${desc:-}" | xargs)"
        [ -z "$name" ] && continue
        cdir="$idf/components/$name"; ver=""; status="provided-by-idf"
        # IDF 组件目录名可能用连字符（如 esp_tls → components/esp-tls）
        alt="${name//_/-}"
        [ -d "$cdir" ] || cdir="$idf/components/$alt"
        [ -d "$cdir" ] || cdir="$(find "$idf/components" -maxdepth 3 -type d \( -name "$name" -o -name "$alt" \) 2>/dev/null | head -1)"
        if [ -n "$cdir" ] && [ -f "$cdir/idf_component.yml" ]; then
            ver="$(sed -n 's/^version:[[:space:]]*"\?\([^"]*\)"\?.*/\1/p' "$cdir/idf_component.yml" | head -1)"
        fi
        [ -n "$ver" ] || ver="$idfv"   # IDF 内置组件无独立版本号：回落到 IDF 版本
        if [ -z "$cdir" ] || [ ! -d "$cdir" ]; then
            status="missing"
            if [ "$req" = "required" ]; then
                err "IDF 组件缺失：$name"
                [ "$DEPS_STRICT" = "1" ] && DEPS_FAIL=1
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

write_manifest() { # $1=out dir, $2=tc id, $3=compiler, $4=cc ver, $5=target, $6=idf ver, $7=cflags
    local out="$1" tcid="$2" cc="$3" ccver="$4" target="$5" idfver="$6" cflags="$7"
    cat > "$out/toolchain.json" <<EOF
{
  "sdk": "oneye-dev-sdk",
  "sdk_version": "$(sed -n 's/^#define ONEYE_DEV_SDK_VERSION_MAJOR \([0-9]*\)$/\1/p' "$SDK_DIR/include/oneye_dev_sdk.h" | head -1).$(sed -n 's/^#define ONEYE_DEV_SDK_VERSION_MINOR \([0-9]*\)$/\1/p' "$SDK_DIR/include/oneye_dev_sdk.h" | head -1).$(sed -n 's/^#define ONEYE_DEV_SDK_VERSION_PATCH \([0-9]*\)$/\1/p' "$SDK_DIR/include/oneye_dev_sdk.h" | head -1)",
  "sdk_version_str": "$SDK_VERSION",
  "toolchain_id": "$tcid",
  "compiler": "$cc",
  "compiler_version": "$ccver",
  "target": "$target",
  "idf_version": "$idfver",
  "cflags": "$cflags",
  "built_at_utc": "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
}
EOF
    ( cd "$out" && find lib include -type f 2>/dev/null | sort | xargs -r sha256sum > SHA256SUMS )
}

# --------------------------------------------------------------- 构建：宿主
build_host() {
    local out="$OUT_ROOT/$(host_cc >/dev/null; local c; c="$(host_cc)"; echo "$("$c" -dumpmachine)-gcc-$(cc_full_version "$c")")"
    local scratch="$OUT_ROOT/.build/host"
    local c cflags
    c="$(host_cc)"
    cflags="-O2 -Wall -Wextra -fPIC -ffunction-sections -fdata-sections -std=c99"
    info "构建宿主工具链：$("$c" --version | head -1)"
    mkdir -p "$out/lib" "$out/include" "$scratch"
    cp -f "$SDK_DIR"/include/*.h "$out/include/"

    "$c" $cflags -I"$SDK_DIR/include" -c "$SDK_DIR/src/oneye_dev_sdk.c" -o "$scratch/oneye_dev_sdk.o" \
        || { err "编译失败（host）"; return 1; }
    ar rcs "$out/lib/liboneye_dev_sdk.a" "$scratch/oneye_dev_sdk.o" || return 1
    ok "lib/liboneye_dev_sdk.a  ($(stat -c%s "$out/lib/liboneye_dev_sdk.a") bytes)"

    local sover="0" 
    "$c" -shared "$scratch/oneye_dev_sdk.o" -Wl,-soname,"liboneye_dev_sdk.so.$sover" \
        -o "$out/lib/liboneye_dev_sdk.so.$SDK_VERSION_NUM" || { err "链接 .so 失败"; return 1; }
    ln -sf "liboneye_dev_sdk.so.$SDK_VERSION_NUM" "$out/lib/liboneye_dev_sdk.so.$sover"
    ln -sf "liboneye_dev_sdk.so.$sover" "$out/lib/liboneye_dev_sdk.so"
    ok "lib/liboneye_dev_sdk.so.$SDK_VERSION_NUM  ($(stat -c%s "$out/lib/liboneye_dev_sdk.so.$SDK_VERSION_NUM") bytes)"

    bundle_deps_host "$out"
    write_manifest "$out" "$(basename "$out")" "$c" "$(cc_full_version "$c")" "x86_64/host" "" "$cflags"
    [ "$PUBLISH_REPO_LIB" = "1" ] && publish_to_repo_lib "$out" "$(basename "$out")"
    LAST_HOST_OUT="$out"
    LAST_HOST_ID="$(basename "$out")"
    return 0
}

# ------------------------------------------------------- 构建：ESP 目标
build_esp() { # $1=idf 版本, $2=target
    local idfv="$1" target="$2" idf cc
    idf="$IDF_ROOT/esp-idf-$idfv"
    if [ ! -d "$idf" ]; then
        warn "跳过 $target@$idfv：未找到 $idf（先安装或改 --idf-root）"
        return 2
    fi
    cc="$(esp_cc_for "$idf" "$target")"
    if [ -z "$cc" ] || [ ! -x "$cc" ]; then
        warn "跳过 $target@$idfv：未找到交叉编译器（先在该 IDF 下执行 install.ps1/install.sh）"
        return 2
    fi
    local tcid; tcid="$(basename "$cc" | sed 's/-gcc$//')-gcc-$("$cc" -dumpfullversion 2>/dev/null || "$cc" -dumpversion)"
    local out="$OUT_ROOT/$tcid" scratch="$OUT_ROOT/.build/$tcid"
    # xtensa 目标必须加 -mlongcalls，否则应用工程链接期会报
    # "dangerous relocation: call8: call target out of range"（libc/ROM 调用超出 call8 范围）
    local arch_flags=""
    case "$target" in
        esp32|esp32s2|esp32s3) arch_flags="-mlongcalls" ;;
    esac
    local cflags="-O2 -Wall -Wextra -ffreestanding -fno-common $arch_flags -ffunction-sections -fdata-sections -std=c99"
    info "构建 $target @ ESP-IDF v$idfv → $tcid"
    mkdir -p "$out/lib" "$out/include" "$scratch"
    cp -f "$SDK_DIR"/include/*.h "$out/include/"

    "$cc" $cflags -I"$SDK_DIR/include" -c "$SDK_DIR/src/oneye_dev_sdk.c" -o "$scratch/oneye_dev_sdk.o" \
        || { err "编译失败（$tcid）"; return 1; }
    "$(dirname "$cc")/$(basename "$cc" | sed 's/gcc$/ar/')" rcs "$out/lib/liboneye_dev_sdk.a" "$scratch/oneye_dev_sdk.o" \
        || ar rcs "$out/lib/liboneye_dev_sdk.a" "$scratch/oneye_dev_sdk.o" || return 1
    ok "lib/liboneye_dev_sdk.a  ($(stat -c%s "$out/lib/liboneye_dev_sdk.a") bytes)"

    bundle_deps_esp "$out" "$idf" "v$idfv"
    write_manifest "$out" "$tcid" "$cc" "$("$cc" -dumpfullversion 2>/dev/null || "$cc" -dumpversion)" "$target" "v$idfv" "$cflags"
    [ "$PUBLISH_REPO_LIB" = "1" ] && publish_to_repo_lib "$out" "$tcid"
    ESP_OUTS+=("$tcid|$target|$idfv")
    return 0
}

# 把库同步到 SDK 仓内的 lib/<tcid>/（ESP-IDF 组件按此路径优先链接预编译库）
publish_to_repo_lib() {
    local out="$1" tcid="$2"
    mkdir -p "$SDK_DIR/lib/$tcid"
    cp -f "$out/lib/"*.a "$SDK_DIR/lib/$tcid/" 2>/dev/null || true
    cp -f "$out/lib/"*.so* "$SDK_DIR/lib/$tcid/" 2>/dev/null || true
    cp -f "$out/toolchain.json" "$SDK_DIR/lib/$tcid/" 2>/dev/null || true
    cp -f "$out/SHA256SUMS" "$SDK_DIR/lib/$tcid/" 2>/dev/null || true
}

# ------------------------------------------------------------ 构建：demo
build_demo_host() { # $1=out dir（宿主工具链目录）
    local out="$1"
    local tcid; tcid="$(basename "$out")"
    local demo="$OUT_ROOT/demo/$tcid"
    local c cflags
    [ "$WITH_DEMO" = "1" ] || return 0
    c="$(host_cc)"
    cflags="-O2 -Wall -Wextra -std=c99"
    mkdir -p "$demo"
    info "构建并运行宿主 demo → output/demo/$tcid/"
    "$c" $cflags -I"$out/include" "$SDK_DIR/examples/linux/demo.c" "$out/lib/liboneye_dev_sdk.a" -o "$demo/demo_static" || return 1
    ok "demo_static（链接 .a）"
    "$c" $cflags -I"$out/include" "$SDK_DIR/examples/linux/demo.c" -L"$out/lib" -loneye_dev_sdk \
        -Wl,-rpath,"$out/lib" -o "$demo/demo_shared" || return 1
    ok "demo_shared（链接 .so，rpath 指向 output/<tc>/lib）"
    "$c" $cflags -I"$out/include" "$SDK_DIR/examples/linux/demo_dlopen.c" -ldl -o "$demo/demo_dlopen" || return 1
    ok "demo_dlopen（运行期加载）"

    "$demo/demo_static" > "$demo/demo_static.log" 2>&1 && ok "demo_static 运行通过（日志 demo_static.log）" || warn "demo_static 运行失败（见 demo_static.log）"
    "$demo/demo_shared" > "$demo/demo_shared.log" 2>&1 && ok "demo_shared 运行通过（日志 demo_shared.log）" || warn "demo_shared 运行失败（见 demo_shared.log）"
    "$demo/demo_dlopen" "$out/lib/liboneye_dev_sdk.so" > "$demo/demo_dlopen.log" 2>&1 && ok "demo_dlopen 运行通过（ABI 自检）" || warn "demo_dlopen 运行失败（见 demo_dlopen.log）"
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
    local build_dir="$OUT_ROOT/.build/esp-hello_oneye-$tcid"   # 构建目录放在 output/.build 下，避免污染仓库（子仓 git status 保持干净）
    (
        set +u
        # shellcheck disable=SC1090
        . "$idf/export.sh" >/dev/null 2>&1
        # 让例程工程找到 SDK 组件（组件在 SDK 仓根，独立检出时用该环境变量）
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
        ok "hello_oneye 构建通过；组件自动链接预编译库"
    else
        warn "hello_oneye 构建完成，但日志未见 '链接预编译库'（可能回落到源码构建）"
    fi
}

# ---------------------------------------------------------------- 主流程
info "oneye-dev-sdk 统一编译：SDK=$SDK_DIR (v$SDK_VERSION)"
info "输出根：$OUT_ROOT"
mkdir -p "$OUT_ROOT/demo" "$OUT_ROOT/.build"
[ "$DO_CLEAN" = "1" ] && { info "清理输出"; rm -rf "$OUT_ROOT"/*; mkdir -p "$OUT_ROOT/demo" "$OUT_ROOT/.build"; }

# --deps-list：只打印依赖声明（函数已定义完毕，放在此处避免 "command not found"）
if [ "$DEPS_LIST_ONLY" = "1" ]; then
    echo "SDK: $SDK_DIR (version $SDK_VERSION)"
    print_deps_list
    exit 0
fi

# 解析工具链列表
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
LAST_HOST_OUT=""; LAST_HOST_ID=""; ESP_OUTS=()
IFS=',' read -ra SPECS <<< "$TOOLCHAINS"
for spec in "${SPECS[@]}"; do
    spec="$(echo "$spec" | xargs)"
    [ -n "$spec" ] || continue
    case "$spec" in
        host)
            build_host; rc=$?; [ $rc -ne 0 ] && LASTRC=$rc
            ;;
        *@*)
            build_esp "${spec#*@}" "${spec%@*}"; rc=$?; [ $rc -eq 1 ] && LASTRC=1
            ;;
        *) warn "无法识别的工具链规格：$spec（用 host 或 <target>@<idf-version>）" ;;
    esac
done

# demo（库构建完成后再跑，确保组件能取到 output 里的库）
[ "$WITH_DEMO" = "1" ] && [ -n "$LAST_HOST_OUT" ] && build_demo_host "$LAST_HOST_OUT"
for entry in "${ESP_OUTS[@]:-}"; do
    [ -n "$entry" ] || continue
    IFS='|' read -r _tcid _t _v <<< "$entry"
    build_demo_esp "$_v" "$_t"
done

# ------------------------------------------------------------ 汇总报告
REPORT="$OUT_ROOT/BUILD-REPORT.md"
{
    echo "# oneye-dev-sdk 构建报告"
    echo
    echo "- SDK 版本：\`$SDK_VERSION\`（源码：\`$SDK_DIR\`）"
    echo "- 时间：$(date -u +%Y-%m-%dT%H:%M:%SZ)（UTC）"
    echo "- 输出根：\`$OUT_ROOT\`"
    echo
    echo "## 工具链 × 产物"
    echo
    echo "| 工具链目录 | 编译器 | 目标/IDF | 库产物 | 校验值 |"
    echo "| --- | --- | --- | --- | --- |"
    for d in "$OUT_ROOT"/*/; do
        [ -f "$d/toolchain.json" ] || continue
        tcid="$(basename "$d")"
        ccv="$(sed -n 's/.*"compiler_version": "\(.*\)".*/\1/p' "$d/toolchain.json")"
        tgt="$(sed -n 's/.*"target": "\(.*\)".*/\1/p' "$d/toolchain.json")"
        idfv="$(sed -n 's/.*"idf_version": "\(.*\)".*/\1/p' "$d/toolchain.json")"
        libs="$(cd "$d/lib" && ls -1 2>/dev/null | tr '\n' ' ')"
        sums="$(cd "$d" && sha256sum lib/*.a 2>/dev/null | awk '{print substr($1,1,16)}' | tr '\n' ' ')"
        echo "| \`$tcid\` | $ccv | $tgt ${idfv:-（host）} | $libs | $sums |"
    done
    echo
    echo "## 依赖打包（随产物复制/登记；声明见 components/oneye-dev-sdk/deps/）"
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
    echo "## demo 产物"
    echo
    echo "| 工具链目录 | 产物 |"
    echo "| --- | --- |"
    for d in "$OUT_ROOT"/demo/*/; do
        [ -d "$d" ] || continue
        files="$(cd "$d" && ls -1 | tr '\n' ' ')"
        echo "| \`$(basename "$d")\` | $files |"
    done
} > "$REPORT"

info "完成。产物树："
( cd "$OUT_ROOT" && find . -maxdepth 2 -mindepth 1 \( -name '.build' -prune -o -print \) | sort | sed 's/^/    /' )
info "汇总报告：$REPORT"
[ "$DEPS_FAIL" = "1" ] && { err "存在 required 依赖缺失（--deps-strict 生效），构建判定失败"; LASTRC=1; }
exit $LASTRC
