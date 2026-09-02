# 对象池语义线索收集记录

记录时间：2026-09-01（最后更新 2026-09-02）  
样本：zip Android ARM64 `/tmp/opencode/zip_test/extract/libapp.so`；winapp Flutter Windows x64 `/tmp/opencode/winapp/app.so`  
产物：hex 过滤前 `/tmp/opencode/zip_test/out/`；hex+短名 `/tmp/opencode/zip_test/out_hexfilter/`；噪声过滤 `/tmp/opencode/zip_test/out_noisefilter2/`（zip）、`/tmp/opencode/winapp/out_noisefilter/`（winapp）；回归脚本输出 `/tmp/opencode/{zip_test,winapp}/out_regress/`  
HEAD：`8466682`（噪声过滤已提交，黑名单补充与回归脚本未提交）  
约束：只在 `// semantic:` 注释和 `strings_to_funcs.txt` 交叉表里加线索，不改真实符号名。  
噪声过滤规格：`.monkeycode/specs/2026-09-01-semantic-clue-noise-filter/`  
回归脚本：`scripts/regression.sh`

## 1. 当前实现（尚未扩收集）

落地位置：

- `blutter/src/DartDumper.cpp`：`isSemanticString`、`tryGetPoolString`、`DumpCode` 收集循环、`DumpStringCrossRef`、`getPoolObjectDescription`、`ObjectToString`
- `blutter/src/DartDumper.h`：`tryGetPoolString`、`stringToFuncs`、`DumpStringCrossRef`
- `blutter/src/main.cpp`：CLI `-i`/`-o`；`DumpCode` 之后调用 `DumpStringCrossRef`
- `blutter/src/CodeAnalyzer.h`：`AsmText::DataType` = `None` / `ThreadOffset` / `PoolOffset` / `Boolean` / `Call`（`callAddress`）

`DumpCode` 现状（约 486–508 行）：只扫 `AsmText::PoolOffset`，经 `tryGetPoolString` + `isSemanticString` 去重后写入函数头：

```
// semantic: "a", "b", ...
```

每函数最多 16 条，超出写 `, ...`。引用同时记入 `stringToFuncs`，最后由 `DumpStringCrossRef` 写成 `strings_to_funcs.txt`。

`tryGetPoolString` 只接受 `kTaggedObject` + `IsString()`，返回 `getQuoteString`（带双引号、已 unescape）。非 String 对象不会进 semantic。

`getQuoteString`：按对象指针缓存；`Util::UnescapeWithQuote(obj.ToCString())`；空串表示未插入。

## 2. `isSemanticString` 现过滤与 hex 漏洞

现规则（对带引号字面量）：

- 总长至少 4（含引号），剥引号后内容长度 2–80
- 必须首尾是 `"`
- `alpha`（`isalpha` 或 `_`）< 2 则丢
- `digit > alpha * 2` 视为哈希/数字堆，丢
- `other > alpha` 且无空格，视为标点包裹，丢
- `\\` 转义计为 `other`，并跳过下一字符

漏洞：`std::isalpha` 把 `a–f` / `A–F` 当字母。椭圆曲线参数、UUID 类 hex blob 的 “字母” 足够多，过不了 `digit > alpha*2`。

zip 产物里约 122 条此类噪声。典型：

- `yXo.dart`：`"c302f41d932a36cda7a3463093d18db78fce476de1a86297"` + `"brainpoolp192t1"`
- `qHo.dart`：`"fffffffffffffffffffffffffffffffffffffffffffffffffffffffefffffc2f"` + `"secp256k1"`
- `rHo.dart`：`"ffffffff00000001000000000000000000000000ffffffffffffffffffffffff"` + `"secp256r1"`
- `WXo.dart` / `VXo.dart`：`"db7c2abf62e35e668076bead208b"` + `"secp112r2"` / `"secp112r1"`

曲线名（`secp256k1`、`brainpoolp192t1`、`prime192v1`）本身是有用线索，应保留；纯 hex 应丢。

拟定收紧（尚未改代码）：

- 剥引号后，若几乎全是 `[0-9a-fA-F]` 且长度 >= 16（或 hex 比例很高、几乎无元音/下划线/空格），视为 hex blob 丢弃
- `a–f` 同时计入 hex 计数，不再只当 `alpha`
- 保留含空格的句子、camelCase、带 `_` 的标识符、曲线名这类混合 token

## 3. zip 对象池规模（pp.txt）

| 类型 | 约数量 | 备注 |
|------|--------|------|
| String | 46122 | 主线索源，已进 semantic |
| Field | 2699 | 多数混淆名 |
| Type | 1885 | `Type: ClassName` / 带 type args |
| Function | 275 | 含 dart:io / dart:core |
| UnlinkedCall | 393 | 几乎全是 `SwitchableCallMissStub` |

Windows `app.so` 对照：恢复 10877 函数；交叉表 26245 字符串 / 34919 引用；1072 文件 / 3513 条注释。  
zip ARM64：恢复 7892 函数；交叉表 28690 / 36586；907 文件 / 3021 条注释。

## 4. 高价值对照样本

### 4.1 已有字符串线索（扩收集后应仍在）

`asm/Hip.dart` `_hFk`（addr `0xac0210`）现已有 1 条 semantic：

```
// semantic: "Running vpn on endpint ", " enhanced mode ", "proxyIp", "proxyPort", "apps", "appsBypass", "enhancedMode", "startVpn"
```

`cCp.dart`：`"MD5"` / `"SHA-256"`。

`List<String>` 字面量多数已作为独立 `PoolOffset` 字符串进 semantic，单独抽数组收益有限。

### 4.2 dart:io Function（对象池，尚未进 semantic）

`pp.txt` 形态：`Function: [dart:io] Class::name (0x...)`

已见：

- `[dart:io] _ExternalBuffer::start`
- `[dart:io] _SecureFilterImpl::buffers`
- `[dart:io] _SocketControlMessageImpl::level`
- `[dart:io] _ResourceHandleImpl::_ResourceHandleImpl`（构造函数，价值低于上面几个）

`ObjectToString` 对 `kFunctionCid`：`app.GetFunction(entry_point - base)`；stub / 找不到则 `[unknown]`；闭包走 `AnonymousClosure`；否则 `Function: {FullName} ({addr})`。

`FullName` 形态：`[lib.url] Class::name`。

### 4.3 Field（对象池，尚未进 semantic）

`ObjectToString` `kFieldCid`：`field.ToCString()` + `TargetOffset`，pp.txt 形态：

```
Field <ClassName.fieldName>: late (offset: 0xc)
Field <_GrowableList@0150898._Vm@0150898>: static late final (offset: 0x0)
```

`DartField` / `DartFunction` 名字来自 `UserVisibleNameCString()`。

2217 unique Field 名里约 458 看起来像词。可读例：`_port` / `_name` / `_index` / `_data` / `_context` / `_current`。  
多数仍是 `_adg` / `_krd` / `_VTl` 这类混淆，单独当线索价值低。

过滤建议：只要 Field 短名（`UserVisibleName` 或 `ToCString` 里点号后、`@` 前那段）像标识符且含元音/常见词根；丢 `@数字` 混淆、1–3 字母乱码、纯 `_xxY`。

### 4.4 Type（对象池，尚未进 semantic）

`ObjectToString`：`Type: ` + `typeDb->FindOrAdd(...)->ToString()`。  
`DartType::ToString` = `cls.Name()` + type args。

有意义的是 dart:core / dart:io / 应用侧可读类名。混淆短名（`Teb`、`bHa`）价值低。

### 4.5 明确低价值、不要当线索

- `UnlinkedCall` → `SwitchableCallMissStub`
- NativeFn `[no name]`
- Field/Type/Function 的混淆短名（`_adg`、`_VTl`、`Teb`）
- Stub（`GetFunction` 返回 `IsStub()`，或 `AllocateClosureStub` 等）
- Shader / 内部 stub 名
- 纯 hex / UUID / 曲线参数数字

## 5. 扩收集接口（已核对，尚未改代码）

`DumpCode` 里每条 `AsmText` 已有：

| dataType | 现用途 | 扩收集用法 |
|----------|--------|------------|
| `PoolOffset` | `tryGetPoolString` 只取 String；反汇编 extra 用 `getPoolObjectDescription` | 同一 offset 再看 cid：Field / Type / Function |
| `Call` | extra = `fn->FullName()` + 可选返回类型 | 非 stub 的 `FullName` 或短名可进 semantic |
| `ThreadOffset` / `Boolean` | 只写 extra | 不收集 |

`app.GetFunction(addr)`（`DartApp.cpp:96`）：先 `functions`，再 `stubs`，再按区间 Split stub；找不到返回 `nullptr`。`DartDumper` 是 `DartApp` friend。

`Call` 路径已在 DumpCode 527–537 行解析过一次，扩收集可在写头注释的第一趟循环里并行扫 `AsmText::Call`，不必再走对象池。

非 String 的对象池条目不要复用 `tryGetPoolString`（它硬编码 `IsString()`）。两条路：

1. 扩 `tryGetPoolString` 为 `tryGetPoolClue`，按 cid 返回规范化短线索
2. 在收集循环里直接 `pool.ObjectAt` + `ObjectToString` / Field/Function API，再过一层 `isUsefulClue`

推荐 2：String 路径保持 `getQuoteString` + `isSemanticString`；Field/Type/Function/Call 另做短名提取，避免把 `Field <...>: late (offset: 0xc)` 整段塞进注释。

线索输出格式建议（仍全部进同一 `// semantic:` 行，去重、最多 16）：

- String：保持 `"proxyIp"`（带引号）
- Field：`field:_port` 或 `_port`（若已有同名字符串则去重）
- Type：`type:SecureSocket` 或类短名
- Function / Call：`call:_ExternalBuffer::start` 或 `[dart:io] _ExternalBuffer::start`

`stringToFuncs` 目前 key 是带引号字符串。非 String 线索若也要进交叉表，key 用同一规范化文本；或交叉表仍只记 String，非 String 只出现在函数头。倾向：交叉表继续只记 String，避免把 `field:_port` 和 `"port"` 混成两种 key。

## 6. 回归对照（改完后必看）

输入：`/tmp/opencode/zip_test/extract/libapp.so`  
输出：新目录（不要覆盖 `/tmp/opencode/zip_test/out/`，便于 diff）

必看：

1. `Hip.dart` `_hFk`：原 8 条字符串仍在；允许额外 Field/Call，不能丢 `"startVpn"` / `"proxyIp"`
2. `yXo.dart` / `qHo.dart` / `rHo.dart`：hex blob 从 semantic 消失；`secp256k1` / `brainpoolp192t1` 等曲线名仍在
3. 若某函数对象池/调用里有 dart:io Function，头注释应出现 `start` / `buffers` / `level` 一类短名
4. 交叉表字符串数应下降（hex 被滤掉），函数引用数不必涨（非 String 不进交叉表）

## 7. 本地构建注意

- 本地 arm64 语义回归：手动 cmake，C/C++ 都用 clang-16 + `-stdlib=libc++`，不可走 `build.py` aarch64 交叉编译
- cmake 宏：`-DDARTLIB=dartvm3.3.4_android_arm64 -DBLUTTER_ARCH=arm64 -DOLD_MAP_NO_IMMUTABLE=1 -DHAS_RECORD_TYPE=1`
- 现成目录：`/tmp/opencode/ci/build/blutter_arm64_dbg/`（有 ninja）；另有 `/tmp/opencode/workspace_build/blutter_dartvm3.3.4_android_arm64`
- 构建必须用 background terminal（编译 memory_percent 视峰值；完整解析 timeout 拉长），成功标志：EXIT=0 且出现 `Generating Frida script`
- CLI：`-i` / `-o`，位置参数不对
- `packages/`、`scripts/__pycache__/` 不入库（已在 `.gitignore`）；提交推送按用户指示执行，不主动切分支

## 8. 已落地（hex 过滤 + 短名 + 噪声过滤）

1. `isHexBlob`：剥引号后长度 ≥16 且全 `isxdigit` 则丢；曲线名因含非 hex 字母保留
2. `DumpCode` 三趟：String（进交叉表）→ Field/Type/Function → 非 stub Call
3. 噪声过滤（`DartDumper.cpp`）：`CALL_BLACKLIST` / `TYPE_BLACKLIST`；`dart:core` Call 全丢；方法名以 `_` 开头丢；Type≤2 / Call≤4
4. zip 回归 `out_noisefilter2/`：EXIT=0，`Generating Frida script`；交叉表 28602 / 36466（key 集合与过滤前 md5 一致）；`$obfuscated::__unknown_function__` / `_StringBase::_interpolate` / `type:String`/`List`/`bool`/`Object` = 0；`Hip.dart` `_hFk` 原 8 条字符串 + `field:_port` 仍在；`call:_ExternalBuffer::start` 出现 3 次
5. 黑名单补充（2026-09-02）：`_fw::call`/`_dw::call`（混淆 async/stream call 包装，两样本各自最高频噪声）、`scheduleMicrotask`、`_SecureFilterImpl::buffers`、`_SocketControlMessageImpl::level`、`allocateOneByteString`、`_AsyncStarStreamController::addStream`/`add`、`_StreamController::Am`、`_Future::timeout`、`_Completer::Bod`
6. winapp（Flutter Windows x64）回归 `out_noisefilter/`：x64 构建（`BLUTTER_ARCH=x64` + `DARTLIB=dartvm3.3.4_linux_x64` + `NO_FRIDA=1`）解析，EXIT=0；`// semantic:` 3825 行；黑名单全 0（含 `_dw::call`）；`field:_port` 16、`call:DynamicLibrary::ebd` 18（dart:ffi 业务线索保留）
7. 回归脚本 `scripts/regression.sh`：`--no-build zip|winapp|all`；固定断言黑名单=0、交叉表行数（zip 93673）、业务线索保留（zip `_ExternalBuffer::start`/`startVpn`/`field:_port`；winapp `DynamicLibrary`），当前 PASS=53
