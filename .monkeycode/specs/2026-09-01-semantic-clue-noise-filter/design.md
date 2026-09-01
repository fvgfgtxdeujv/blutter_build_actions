# 技术设计：semantic 线索噪声过滤

- 日期：2026-09-01
- 对应需求：`requirements.md`（同目录）
- 代码位置：`blutter/src/DartDumper.cpp`（不涉及 `DartApp` 层 / 符号名）

## 1. 现状与数据形态

- 收集循环 `DumpCode`（`DartDumper.cpp` 约 574–627 行）三趟：
  1. pass 1：`PoolOffset` → `tryGetPoolString` + `isSemanticString`，进交叉表
  2. pass 2：`PoolOffset` → cid 分支 Field/Type/Function → `fieldClueFromCString` / `typeClueFromName` / `callClueFromFn`
  3. pass 3：`AsmText::Call` → `app.GetFunction(addr)` → `callClueFromFn`
- 全部线索进同一 `clues` 向量，`addClue` 去重（`seen`），写头注释时截断到 16 条（约 628–639 行）。
- `callClueFromFn`（153–178 行）现产出 `call:cls::method` / `call:method`；`FullName` = `[lib.url] Class::name`（`DartFunction.cpp:159-163`）。
- 噪声量化（`out_hexfilter/`）：Call 3736（`$obfuscated::__unknown_function__` 1585、`_StringBase::_interpolate` 659、`Shader::Shader._` 133、`LateError::_throwFieldAlreadyInitialized` 126）；Type 737（`String` 302、`List` 174、`bool` 81、`Object` 50）。

## 2. 设计原则

- 全静态规则，无学习/状态；优先级顺序即处理顺序，保证可预期、可回归。
- 保 String、Field，优先砍 Call、Type —— 前者是高价值业务线索，后者是噪声来源。
- 黑名单只作用于短名（`cls::method` 或 `cls` / `method` 单侧），库 URL 作为辅助信号，不参与语义行输出。
- 不改 `stringToFuncs` 结构：交叉表仍只记 String。

## 3. 具体修改点

### 3.1 静态黑名单表

文件头部新增两个 `static const std::set<std::string>`（或 `std::unordered_set<std::string>`）：

```cpp
// Full-name (cls::method) or single-ident call/type names that are pure noise.
static const std::unordered_set<std::string> CALL_BLACKLIST = {
	"$obfuscated::__unknown_function__",
	"$obfuscated::_ffi_resolver_function",
	"Shader::Shader._",
	"LateError::_throwFieldAlreadyInitialized",
	"Native::_ffi_resolver_function",
};
static const std::unordered_set<std::string> TYPE_BLACKLIST = {
	"String", "List", "bool", "Object", "void",
};
```

- 精确匹配短名（不拆库 URL）；`_StringBase::_interpolate` 走 `dart:core` 规则（3.3），不进表。

### 3.2 `callClueFromFn` 增加过滤

在现有 `isUsefulIdent` 检查后、返回前插入：

```cpp
// drop dart:core function calls (StringBase::_interpolate etc.)
auto rb = full.find(']');
if (rb != std::string::npos) {
	std::string_view url = std::string_view(full).substr(1, rb - 1);
	if (url == "dart:core")
		return {};
}
```

完整短名检查（`call:` 前缀已剥掉）：

```cpp
std::string shortName = cls.empty() ? method : cls + "::" + method;
if (CALL_BLACKLIST.count(shortName))
	return {};
```

构造/初始化形态（在 `method == cls` 分支之后）：

```cpp
// Class._ / Class._1 constructor-ish initializers and private methods
if (!method.empty() && method[0] == '_')
	return {};
if (!cls.empty() && cls[0] == '_') {
	// private class: keep only a public-looking method
	if (method == cls) return {}; // already handled
	if (method[0] != '_' && method.find('.') != std::string::npos) return {}; // tear-off
}
```

注意：`cls` 本身以 `_` 开头时（`_ExternalBuffer`），方法名 `start` / `buffers` 不带 `_` 且不是构造，应保留（`_ExternalBuffer::start` 是有价值 dart:io 线索）。因此上面的 `cls[0] == '_'` 分支只丢 `method` 以 `_` 开头或 `method == cls` 的构造/初始化，不丢 `start` / `buffers`。

dart:core 里 `dart:core` 的 `_StringBase::_interpolate` 走 URL 规则直接丢，无需黑名单。

### 3.3 `typeClueFromName` 增加过滤

在 `isUsefulIdent` 检查后、返回前：

```cpp
if (TYPE_BLACKLIST.count(std::string(typeName)))
	return {};
```

### 3.4 收集循环增加优先级与限额

收集循环（约 574–627 行）改成分桶收集：

```cpp
std::vector<std::string> clues;          // final output order: String, Field, Type, Call
std::vector<std::string> typeClues, callClues;   // subject to caps
```

- pass 1 String：`addClue` 直接进 `clues`（同现状，进交叉表）。
- pass 2/3 分类：
  - Field 线索（`field:...`）直接进 `clues`。
  - Type 线索进 `typeClues`，最后只取前 2 条；Call 线索进 `callClues`，最后只取前 4 条。
- 汇总（顺序固定，保证 `Hip.dart` 的 String 在前）：

```cpp
if (typeClues.size() > 2) typeClues.resize(2);
if (callClues.size() > 4) callClues.resize(4);
clues.insert(clues.end(), typeClues.begin(), typeClues.end());
clues.insert(clues.end(), callClues.begin(), callClues.end());
```

- 写头注释的 16 条截断保持现状（`std::min(clues.size(), size_t{16})` + `, ...`）。限额已先把 Call 压到 ≤4 / Type ≤2，String/Field 多的函数仍可能触发 16 截断，属预期。

### 3.5 库 URL 提取辅助

`DartLibrary::Url()`（`DartLibrary.h:22`）返回如 `dart:core` / `dart:io` / `package:xxx`。`FullName` 前缀固定为 `[url] `，解析用现有 `find(']')` 即可，不新增接口。

## 4. 代码改动清单（仅 DartDumper.cpp）

1. 头部新增 `CALL_BLACKLIST` / `TYPE_BLACKLIST` 两个静态表
2. `callClueFromFn`：dart:core URL 丢弃、黑名单短名丢弃、`method[0]=='_'` 丢弃、`cls[0]=='_'` 时丢构造/初始化（保留 `_ExternalBuffer::start`）
3. `typeClueFromName`：TYPE_BLACKLIST 丢弃
4. `DumpCode` 收集循环：分桶 Type/Call + 限额 + 汇总顺序

不涉及：`DartDumper.h`、`main.cpp`、`CodeAnalyzer.h`、`DartApp*`、`DartFnBase*`、`DartFunction*`。

## 5. 预期效果（对照 out_hexfilter/）

| 指标 | out_hexfilter/ | 本设计预期 |
|------|---------------|-----------|
| semantic 行内 Call | 3736 | 明显下降：`$obfuscated::__unknown_function__`、`_StringBase::_interpolate`、`Shader::`、`LateError::` 全消失 |
| semantic 行内 Type | 737 | `String`/`List`/`bool`/`Object` 消失 |
| 每函数 Call 上限 | 16 | 4 |
| 每函数 Type 上限 | 16 | 2 |
| `Hip.dart` `_hFk` String 8 条 | 仍在 | 仍在（String 优先级最高且不受限） |
| 交叉表 String 条目 | 28602 | 不增（黑名单只影响 `// semantic:` 行，不进交叉表） |

## 6. 回归与验证

输入：`/tmp/opencode/zip_test/extract/libapp.so`；输出：新目录 `/tmp/opencode/zip_test/out_noisefilter/`（勿覆盖 `out/`、`out_hexfilter/`）。

- 构建：`/tmp/opencode/ci/build/blutter_arm64_dbg/` 手动 cmake（clang-16 + `-stdlib=libc++`），background terminal
- 成功标志：EXIT=0 且出现 `Generating Frida script`
- 必看（对应需求第 5 节）：
  1. `Hip.dart` `_hFk`：`"startVpn"` / `"proxyIp"` 等 8 条仍在；`call:_ExternalBuffer::start` 等业务 Call 可加但不得挤掉 String
  2. 全文 grep：`$obfuscated::__unknown_function__` = 0、`_StringBase::_interpolate` = 0、`type:String` / `type:List` = 0
  3. 曲线名仍在（`secp256k1` 等），纯 hex 不出现
  4. `field:_port` 仍在
  5. 交叉表 String 条目 ≤ 28602
- 统计口径：`semantic` 行数、各前缀条数用 `rg -c` / `awk` 汇总，与 `out_hexfilter/` 对比。

## 7. 风险与备选

- `cls[0]=='_'` 分支可能误伤合法私有类方法（如 `_Foo::bar`，`bar` 不带 `_`）。私有类的方法短名价值本来就低，接受误伤；若回归发现某函数丢失高价值私有类线索，可改为黑名单精确表补充。
- dart:core URL 判断用字符串比较，若出现 URL 变体（`dart:core/...`）需加前缀匹配；当前样本无此形态，先用精确匹配，回归时校验。
- 若限额后某函数仍 >16 条，截断逻辑不变（`...` 兜底），不引入排序策略，保证输出可预测。
