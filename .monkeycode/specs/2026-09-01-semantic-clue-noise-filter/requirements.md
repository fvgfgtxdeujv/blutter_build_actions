# 需求：semantic 线索噪声过滤（黑名单、种类限额、业务 API 过滤）

- 日期：2026-09-01
- 关联代码：`blutter/src/DartDumper.cpp`
- 现状：`DumpCode` 三趟收集（String → Field/Type/Function → 非 stub Call）已落地；hex 过滤与 Field/Type/Call 短名已合并。semantic 行总条目 4582，但 Call 3736 中噪声占比高：`call:$obfuscated::__unknown_function__` 1585、`_StringBase::_interpolate` 659、`Shader::Shader._` 133、`LateError::_throwFieldAlreadyInitialized` 126；Type 737 中 `String` 302、`List` 174、`bool` 81、`Object` 50。这些问题掩盖了 `startVpn` / `proxyIp` 等高价值线索。

## EARS 需求

### Call 黑名单

- **WHEN** `callClueFromFn` 生成 `call:<cls>::<method>` 或 `call:<method>` 短名 **THEN** 匹配以下任一模式时丢弃：
  1. 方法名或类名为 `$obfuscated`、`__unknown_function__`、`_ffi_resolver_function`
  2. 类名以 `_` 开头且方法名为构造/初始化形态（`Class._`、`Class._<digit>*`），或方法名以 `_` 开头
  3. 完整短名（`cls::method`）落在 `CALL_BLACKLIST` 精确表内
  4. 库 URL 为 `dart:core` 的 Call（`_StringBase::_interpolate`、`StringBase` 等）
- **WHEN** 函数头 semantic 行超过 16 条 **THEN** 先按优先级丢弃：黑名单 Call > 普通 Call > Type > Field > String，直到 ≤16 条；超出的位置写 `, ...`

### 种类限额

- **WHEN** 收集单个函数的 semantic 线索 **THEN** Call 最多 4 条（黑名单之外），Type 最多 2 条；String / Field 不受限
- **WHEN** 某个函数产生 ≥5 条 Call 候选 **THEN** 只保留按出现顺序前 4 条

### 业务 API 过滤（Call 保留策略）

- **WHEN** 候选 Call 类名非 `dart:core` 且短名通过黑名单 **THEN** 保留
- **WHEN** 候选 Call 属于 `dart:io` 且短名通过黑名单 **THEN** 保留
- **WHEN** 候选 Call 属于 `dart:core` **THEN** 全部丢弃（不进入 semantic，也不进交叉表）

### Type 过滤

- **WHEN** `typeClueFromName` 生成 `type:<name>` **THEN** 匹配 `TYPE_BLACKLIST`（`String`、`List`、`bool`、`Object`、`void` 等过泛类型）时丢弃

### 回归对照

- **WHEN** 用 zip `libapp.so` 回归 **THEN** 满足以下全部：
  1. `Hip.dart` `_hFk` 原 8 条字符串（含 `"startVpn"` / `"proxyIp"`）仍在，不被 Call/Type 挤掉
  2. 曲线名（`secp256k1` / `brainpoolp192t1`）仍出现，纯 hex 不出现
  3. `call:$obfuscated::__unknown_function__`、`_StringBase::_interpolate` 从 semantic 消失
  4. `field:_port` 等 Field 保留
  5. 交叉表（`strings_to_funcs.txt`）仍只记 String；String 条目数相对 `out_hexfilter/` 不增
- **WHEN** 用 `dart:io` 有调用函数的样本（`Hip.dart` 等） **THEN** 头注释应出现 `call:_ExternalBuffer::start` 类业务 API 短名

## 排除项

- 不改真实符号名、不改 `DartApp` 层函数名
- 不做跨库启发式（例如按类名相似度归并）
- 不把 `field:` / `type:` / `call:` 前缀线索写入 `strings_to_funcs.txt`（交叉表仍只记 String）
- 不做用户输入学习（黑名单为静态表）
