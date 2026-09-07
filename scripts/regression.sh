#!/bin/bash
# 语义线索回归脚本：zip (Android ARM64) + winapp (Flutter Windows x64)
#
# 每个样本：解析 -> 断言 semantic 线索黑名单=0、关键业务线索保留、交叉表行数不变。
# 用法：
#   scripts/regression.sh             # 先构建两个二进制，再跑全部样本
#   scripts/regression.sh --no-build  # 跳过 ninja，只跑解析与断言
#   scripts/regression.sh zip         # 只跑 zip 样本
#   scripts/regression.sh winapp      # 只跑 winapp 样本
# 环境变量：ZIP_SO/WIN_SO/..._OUT_DIR 覆盖样本路径；CANDIDATE_MIN 调整候选频次阈值（默认 5）
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
# 已配套 semantic_candidates()：回归时反向输出高频低信息量候选，见 blacklist_candidates.txt
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

# ---- 黑名单候选统计（黑名单运营半自动化）----
# 扫描 semantic 行中的带前缀线索，按频次降序；剔除保留白名单、出现次数
# >=CANDIDATE_MIN 的条目写入样本输出目录 blacklist_candidates.txt，供人工
# 复核后追加到 blutter/src/semantic_blacklist.txt（信息性报告，不计 PASS/FAIL）
CANDIDATE_MIN="${CANDIDATE_MIN:-5}"
KEEP_CLUES=(
	'field:_port'
)

semantic_candidates() { # <semantic行文件> <报告输出路径> <样本名>
	local semfile="$1" report="$2" name="$3"
	local body kept_join
	body="$(mktemp)"
	kept_join="$(IFS='|'; printf '%s' "${KEEP_CLUES[*]}")"
	grep -a -o -E '(call|field|type|name):[^,"]+' "$semfile" \
		| sort | uniq -c | sort -rn \
		| awk -v keep="$kept_join" -v min="$CANDIDATE_MIN" '
			BEGIN { n = split(keep, k, "|"); for (i = 1; i <= n; i++) kept[k[i]] = 1 }
			{
				cnt = $1 + 0
				line = $0
				sub(/^[[:space:]]*[0-9]+[[:space:]]+/, "", line)
				sub(/[[:space:]]+$/, "", line)
				c = index(line, ":")
				if (c == 0) next
				val = substr(line, c + 1)
				gsub(/[[:space:]]/, "", val)
				if (val == "") next
				if (line in kept) next
				if (cnt < min + 0) next
				printf "%6d  %s\n", cnt, line
			}
		' >"$body"
	local total
	total="$(wc -l <"$body")"
	{
		echo "# [$name] 高频低信息量黑名单候选（>=${CANDIDATE_MIN} 次，已排除保留名单）"
		echo "# 复核后将条目加入 blutter/src/semantic_blacklist.txt 并重跑回归"
		cat "$body"
	} >"$report"
	rm -f "$body"
	if [ "$total" -gt 0 ]; then
		echo "    [$name] 黑名单候选线索 $total 条（Top5）:"
		tail -n +3 "$report" | head -5 | sed 's/^/      /'
	else
		echo "    [$name] 无新增高频线索候选"
	fi
}

PASS=0
FAIL=0
FAIL_MSG=()

assert_zero() { # <描述> <模式文件> <pattern>
	local desc="$1" semfile="$2" pat="$3"
	if grep -a -E -q -- "$pat" "$semfile"; then
		FAIL=$((FAIL + 1))
		FAIL_MSG+=("FAIL  $desc: 命中 '$pat' ($(grep -a -E -c -- "$pat" "$semfile"))")
	else
		PASS=$((PASS + 1))
	fi
}

assert_ge() { # <描述> <文件> <pattern> <最低次数>
	local desc="$1" file="$2" pat="$3" min="$4"
	local n
	n="$(grep -a -E -c -- "$pat" "$file" 2>/dev/null || true)"
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

	semantic_candidates "$tmp" "$out/blacklist_candidates.txt" "$name"
	rm -f "$tmp"
}

# ---------------- 语义重命名闭环断言 ----------------
check_semrename() { # <输出目录> <样本名>
	local out="$1" name="$2"
	local py="$out/ida_script/addNames.py"
	local names_file="$out/ida_script/semantic_names.txt"
	if [ ! -f "$names_file" ]; then
		FAIL=$((FAIL + 1))
		FAIL_MSG+=("FAIL  $name: 缺 $names_file")
		return
	fi
	PASS=$((PASS + 1))

	# 追踪表必须有实际覆盖行（排除 # 头注释）
	local renamed
	renamed="$(grep -a -c -- '-> fn_' "$names_file")"
	assert_ge "$name semantic_names 覆盖行" "$names_file" '-> fn_' 1

	# addNames.py 的 ::fn_ 命名行数与追踪表覆盖行数一致
	local py_n
	py_n="$(grep -a -o -- '::fn_[A-Za-z0-9_]*' "$py" | wc -l)"
	assert_eq "$name addNames.py ::fn_ 行数" "$py_n" "$renamed"

	# SDK 库（dart 前缀）零覆盖：url 含 ':' 的库函数不得出现语义名
	assert_zero "$name SDK 零覆盖" "$py" 'set_name\(0x[0-9a-f]+, "dart[^"]*::fn_'
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
	check_semrename "$out" 'zip'
	echo "    产物: $out"
}

# ---------------- winapp (Flutter Windows x64) ----------------
regress_winapp() {
	local out="$WIN_OUT_DIR"
	local log="$out/run.log"
	mkdir -p "$out"

	run_parse "$X64_BIN" "$WIN_SO" "$out" "$log" || return
	# x64 构建现在生成 Windows 桌面 Frida 脚本（blutter_frida_windows.js）
	check_common "$out" 'Generating Frida script' 'winapp'

	assert_ge 'winapp Frida 脚本常量' "$out/blutter_frida_windows.js" 'const CodeAnchors' 1
	assert_ge 'winapp Frida 非压缩标记' "$out/blutter_frida_windows.js" 'const PointerCompressedEnabled = false' 1
	assert_ge 'winapp Frida 类表' "$out/blutter_frida_windows.js" 'const Classes = \[' 1
	if command -v node >/dev/null 2>&1; then
		if node --check "$out/blutter_frida_windows.js" >/dev/null 2>&1; then
			PASS=$((PASS + 1))
		else
			FAIL=$((FAIL + 1))
			FAIL_MSG+=("FAIL  winapp: blutter_frida_windows.js 语法错误（node --check）")
		fi
	fi

	# winapp 是 dart:ffi 重度样本，DynamicLibrary 业务线索必须保留
	local tmp
	tmp="$(mktemp)"
	grep -a -h '// semantic:' "$out/asm"/*.dart >"$tmp"
	assert_ge 'winapp' "$tmp" 'call:DynamicLibrary' 1
	rm -f "$tmp"

	# gap-4: x64 函数体内 [fp+0x10+8i] 晚绑定为 arg_N（基线 2680，取一半防波动）
	local argbind
	argbind="$(grep -ahE "= arg_[0-9]+" "$out"/asm/*.dart | wc -l)"
	if [ "$argbind" -ge 1300 ]; then
		PASS=$((PASS + 1))
	else
		FAIL=$((FAIL + 1))
		FAIL_MSG+=("FAIL  winapp: arg_N 晚绑定行数 $argbind < 1300")
	fi
	check_semrename "$out" 'winapp'
	echo "    产物: $out"
}

# 跨样本稳定候选（用于 all 模式末尾）
cross_candidates() { # 同时出现在两样本候选报告中的线索才是稳定黑名单候选
	local zf="$ZIP_OUT_DIR/blacklist_candidates.txt" wf="$WIN_OUT_DIR/blacklist_candidates.txt"
	[ -f "$zf" ] && [ -f "$wf" ] || return 0
	local t1 t2 inter
	t1="$(mktemp)"; t2="$(mktemp)"
	tail -n +3 "$zf" | awk '{print $2}' | sort -u >"$t1"
	tail -n +3 "$wf" | awk '{print $2}' | sort -u >"$t2"
	inter="$(comm -12 "$t1" "$t2")"
	rm -f "$t1" "$t2"
	if [ -n "$inter" ]; then
		echo "==> 跨样本稳定黑名单候选（两样本均高频，复核后入黑名单）:"
		printf '    %s\n' $inter
	else
		echo "==> 无跨样本稳定候选"
	fi
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
	cross_candidates
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
