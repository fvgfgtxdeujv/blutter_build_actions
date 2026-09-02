#!/bin/bash
# 语义线索回归脚本：zip (Android ARM64) + winapp (Flutter Windows x64)
#
# 每个样本：解析 -> 断言 semantic 线索黑名单=0、关键业务线索保留、交叉表行数不变。
# 用法：
#   scripts/regression.sh             # 先构建两个二进制，再跑全部样本
#   scripts/regression.sh --no-build  # 跳过 ninja，只跑解析与断言
#   scripts/regression.sh zip         # 只跑 zip 样本
#   scripts/regression.sh winapp      # 只跑 winapp 样本
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR_ARM64="${BLUTTER_ARM64_BUILD_DIR:-/tmp/opencode/ci/build/blutter_arm64_dbg}"
BUILD_DIR_X64="${BLUTTER_X64_BUILD_DIR:-/tmp/opencode/ci/build/blutter_dartvm3.3.4_linux_x64}"
ARM64_BIN="$BUILD_DIR_ARM64/blutter_dartvm3.3.4_android_arm64"
X64_BIN="$BUILD_DIR_X64/blutter_dartvm3.3.4_linux_x64"

ZIP_SO="${ZIP_SO:-/tmp/opencode/zip_test/extract/libapp.so}"
ZIP_OUT_DIR="${ZIP_OUT_DIR:-/tmp/opencode/zip_test/out_regress}"
WIN_SO="${WIN_SO:-/tmp/opencode/winapp/app.so}"
WIN_OUT_DIR="${WIN_OUT_DIR:-/tmp/opencode/winapp/out_regress}"

# 交叉表行数基线（strings_to_funcs.txt），黑名单/限额改动不影响它
ZIP_XREF_LINES=93673

# semantic 行内不得出现的黑名单条目（Call/Type/运行时占位）
BLACKLIST_PATTERNS=(
	'\$obfuscated::__unknown_function__'
	'\$obfuscated::_ffi_resolver_function'
	'_StringBase::_interpolate'
	'type:String'
	'type:List'
	'type:bool'
	'type:Object'
	'type:void'
	'call:_fw::call'
	'call:_dw::call'
	'call:scheduleMicrotask'
	'call:_SecureFilterImpl::buffers'
	'call:_SocketControlMessageImpl::level'
	'call:allocateOneByteString'
	'call:_AsyncStarStreamController::addStream'
	'call:_AsyncStarStreamController::add'
	'call:_StreamController::Am'
	'call:_Future::timeout'
	'call:_Completer::Bod'
)

PASS=0
FAIL=0
FAIL_MSG=()

assert_zero() { # <描述> <模式文件> <pattern>
	local desc="$1" semfile="$2" pat="$3"
	if grep -a -q "$pat" "$semfile"; then
		FAIL=$((FAIL + 1))
		FAIL_MSG+=("FAIL  $desc: 命中 '$pat' ($(grep -a -c "$pat" "$semfile"))")
	else
		PASS=$((PASS + 1))
	fi
}

assert_ge() { # <描述> <文件> <pattern> <最低次数>
	local desc="$1" file="$2" pat="$3" min="$4"
	local n
	n="$(grep -a -c "$pat" "$file" 2>/dev/null || true)"
	if [ "$n" -ge "$min" ]; then
		PASS=$((PASS + 1))
	else
		FAIL=$((FAIL + 1))
		FAIL_MSG+=("FAIL  $desc: '$pat' 期望 >=$min 实际 $n")
	fi
}

assert_eq() { # <描述> <实际> <期望>
	local desc="$1" actual="$2" expect="$3"
	if [ "$actual" -eq "$expect" ]; then
		PASS=$((PASS + 1))
	else
		FAIL=$((FAIL + 1))
		FAIL_MSG+=("FAIL  $desc: 期望 $expect 实际 $actual")
	fi
}

run_parse() { # <二进制> <libapp> <输出目录> <日志文件>
	local bin="$1" so="$2" out="$3" log="$4"
	if [ ! -x "$bin" ]; then
		echo "SKIP  二进制不存在: $bin"
		return 2
	fi
	if [ ! -f "$so" ]; then
		echo "SKIP  样本不存在: $so"
		return 2
	fi
	echo "==> 解析 $so"
	echo "    二进制 $bin"
	rm -f "$log"
	stdbuf -oL -eL "$bin" -i "$so" -o "$out" >"$log" 2>&1
	local rc=$?
	if [ "$rc" -ne 0 ]; then
		FAIL=$((FAIL + 1))
		FAIL_MSG+=("FAIL  解析退出码 $rc: $so")
		return 1
	fi
	PASS=$((PASS + 1))
	return 0
}

check_common() { # <输出目录> <成功标志关键词> <样本名>
	local out="$1" okflag="$2" name="$3"
	local log="$out/run.log"
	echo "==> 断言 $name"

	if ! grep -q "$okflag" "$log"; then
		FAIL=$((FAIL + 1))
		FAIL_MSG+=("FAIL  $name: 日志缺成功标志 '$okflag'")
		return
	fi
	PASS=$((PASS + 1))

	local asm="$out/asm"
	if [ ! -d "$asm" ] || [ -z "$(ls "$asm"/*.dart 2>/dev/null)" ]; then
		FAIL=$((FAIL + 1))
		FAIL_MSG+=("FAIL  $name: asm/ 无产物")
		return
	fi
	PASS=$((PASS + 1))

	local tmp
	tmp="$(mktemp)"
	grep -a -h '// semantic:' "$asm"/*.dart >"$tmp" 2>/dev/null

	local semtotal
	semtotal="$(wc -l <"$tmp")"
	if [ "$semtotal" -eq 0 ]; then
		FAIL=$((FAIL + 1))
		FAIL_MSG+=("FAIL  $name: 无任何 // semantic: 行")
	else
		PASS=$((PASS + 1))
	fi

	local pat
	for pat in "${BLACKLIST_PATTERNS[@]}"; do
		assert_zero "$name" "$tmp" "$pat"
	done

	# 业务/字段线索必须保留
	assert_ge "$name" "$tmp" 'semantic:.*field:_port' 1
	rm -f "$tmp"
}

# ---------------- zip (Android ARM64) ----------------
regress_zip() {
	local out="$ZIP_OUT_DIR"
	local log="$out/run.log"
	mkdir -p "$out"

	run_parse "$ARM64_BIN" "$ZIP_SO" "$out" "$log" || return
	check_common "$out" 'Generating Frida script' 'zip'

	local lines
	lines="$(wc -l <"$out/strings_to_funcs.txt")"
	assert_eq 'zip 交叉表行数' "$lines" "$ZIP_XREF_LINES"

	# 混淆样本业务 API 与字符串保留
	local tmp
	tmp="$(mktemp)"
	grep -a -h '// semantic:' "$out/asm"/*.dart >"$tmp"
	assert_ge 'zip' "$tmp" 'call:_ExternalBuffer::start' 1
	assert_ge 'zip' "$tmp" 'startVpn' 1
	assert_ge 'zip' "$tmp" 'field:_port' 1
	rm -f "$tmp"
	echo "    产物: $out"
}

# ---------------- winapp (Flutter Windows x64) ----------------
regress_winapp() {
	local out="$WIN_OUT_DIR"
	local log="$out/run.log"
	mkdir -p "$out"

	run_parse "$X64_BIN" "$WIN_SO" "$out" "$log" || return
	# NO_FRIDA 构建无 Frida 脚本，成功标志用 application assemblies
	check_common "$out" 'Generating application assemblies' 'winapp'

	# winapp 是 dart:ffi 重度样本，DynamicLibrary 业务线索必须保留
	local tmp
	tmp="$(mktemp)"
	grep -a -h '// semantic:' "$out/asm"/*.dart >"$tmp"
	assert_ge 'winapp' "$tmp" 'call:DynamicLibrary' 1
	rm -f "$tmp"
	echo "    产物: $out"
}

# ---------------- main ----------------
DO_BUILD=1
TARGET=all
for arg in "$@"; do
	case "$arg" in
	--no-build) DO_BUILD=0 ;;
	zip | winapp | all) TARGET="$arg" ;;
	-h | --help)
		sed -n '2,9p' "${BASH_SOURCE[0]}"
		exit 0
		;;
	*) echo "未知参数: $arg" >&2 && exit 2 ;;
	esac
done

if [ "$DO_BUILD" -eq 1 ]; then
	echo "==> 构建二进制"
	if [ "$TARGET" = all ] || [ "$TARGET" = zip ]; then
		ninja -C "$BUILD_DIR_ARM64"
	fi
	if [ "$TARGET" = all ] || [ "$TARGET" = winapp ]; then
		ninja -C "$BUILD_DIR_X64"
	fi
fi

case "$TARGET" in
all)
	regress_zip
	regress_winapp
	;;
zip) regress_zip ;;
winapp) regress_winapp ;;
esac

echo ""
echo "==== 结果: PASS=$PASS FAIL=$FAIL ===="
if [ "$FAIL" -gt 0 ]; then
	printf '%s\n' "${FAIL_MSG[@]}"
	exit 1
fi
exit 0
