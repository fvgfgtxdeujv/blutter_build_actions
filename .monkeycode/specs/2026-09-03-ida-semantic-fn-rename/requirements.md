# 需求文档：IDA 语义函数重命名闭环

## Introduction

blutter 解析混淆 Dart AOT 样本时，已将每个函数引用的语义线索（字符串、call、type）收集进 `// semantic:` 注释与 `strings_to_funcs.txt` 交叉表。但 IDA 侧函数仍显示为混淆短名（如 `Hip::_hFk`）或 `__unknown_function__`，分析者需逐个翻阅注释推断函数用途。

本功能为混淆业务函数生成语义候选名，并由 `addNames.py` 直接以 `set_name` 写入 IDA，把「手动看注释」升级为「自动重命名函数」。

## Glossary

- **SDK 库**：库 URL 含冒号的 Dart 标准库与第三方包库（`dart:core`、`dart:io`、`package:flutter/...`），函数名保留真实符号。
- **业务混淆库**：库 URL 不含冒号的库（混淆样本中 URL 形如 `Hip`、`$obfuscated` 或随机短类名），函数名被混淆器改写为短随机串或丢失。
- **强混淆函数**：函数名属于 `__unknown_function__`、`_ffi_resolver_function` 的混淆函数。
- **短名混淆函数**：剥离全部前导 `_` 后核心长度 ≤ 4 且含大写字母或数字的混淆函数名（`_hFk` → `hFk`、`Snb`、`Teb`），Dart 混淆随机短名普遍满足该特征；全小写真实方法名（`load`、`add`）不满足。
- **语义线索**：DumpCode 阶段按现有 pass1（字符串）/pass3（Call 目标）规则收集、经 `isSemanticString` / `callClueFromFn` 过滤的线索。
- **语义候选名**：由语义线索经净化规则生成的合法 IDA 标识符。
- **原名**：Dump4Ida 现有 `getFunctionName4Ida` 产出名（未覆盖时的显示名）。

## Requirements

### R1 语义候选名生成

**User Story:** AS 恶意样本分析者，I want 混淆函数在 IDA 中直接显示业务语义名，SO that 无需逐个翻阅 `// semantic:` 注释。

#### Acceptance Criteria

1. WHEN DumpCode 为函数收集到 ≥ 1 条字符串线索，系统 SHALL 以其中的「业务形字符串」为第一优先级生成语义候选名；业务形定义为：无空格标识符、小写字母开头、词形可读、长度 3–28、非命名黑名单（`dart_ui`、`id`、`type`、`from`、`start` 等通用词）。
2. WHEN 函数存在多个业务形字符串线索，系统 SHALL 取引用顺序中最后一个作为候选名（`_hFk` 引用 `proxyIp`…`startVpn` 时命名 `fn_startVpn`）。
3. WHEN 函数无业务形字符串但存在 ≥ 1 条 call 线索，系统 SHALL 以 call 线索为第二优先级生成语义候选名。
4. WHEN 函数无任何可用命名线索，系统 SHALL 放弃生成，函数保持原名。
5. WHEN 净化后的候选名长度超过 64 字符，系统 SHALL 截断到 64 字符。
6. WHEN 净化后候选名为空或以数字开头，系统 SHALL 添加 `fn_` 前缀。
7. 系统 SHALL 保证句子型/错误消息/库 url/随机混淆串等非业务字符串仅保留在 `// semantic:` 注释与交叉表，不参与函数命名。

### R2 覆盖范围判定

**User Story:** AS 分析者，I want 真实符号函数名保持不动、仅混淆业务函数被语义覆盖，SO that 不破坏 SDK 符号与脚本地址对应。

#### Acceptance Criteria

1. WHEN 函数所属库 URL 含 `:`（SDK 库），系统 SHALL 保留 `getFunctionName4Ida` 原名，语义候选名不生效。
2. WHEN 函数所属库 URL 不含 `:` 且函数名为强混淆函数，系统 SHALL 以语义候选名覆盖原名。
3. WHEN 函数所属库 URL 不含 `:` 且函数名满足短名混淆特征，系统 SHALL 以语义候选名覆盖原名。
4. WHEN 函数所属库 URL 不含 `:` 且函数名为 `<anonymous closure>`，系统 SHALL 保留 `_anon_closure` 现有命名。
5. WHEN 函数所属库 URL 不含 `:` 且函数名为真实语义名（核心 > 4 或全小写无数字），系统 SHALL 保留原名。
6. WHEN 函数满足覆盖条件但无语义候选名，系统 SHALL 保留原名。

### R3 IDA 脚本输出

**User Story:** AS 分析者，I want `addNames.py` 一次运行完成重命名，SO that IDA 加载即得可读函数名。

#### Acceptance Criteria

1. WHEN Dump4Ida 输出被覆盖函数的 `set_name`，系统 SHALL 使用 `{lib}_{cls}::fn_{token}_{ep:x}` 格式（`ep` 为函数入口地址十六进制，沿用现有唯一性后缀约定）。
2. WHEN Dump4Ida 输出被覆盖函数，系统 SHALL 追加 `idaapi.set_cmt(ep, "origin: {原名}", 0)` 保留原名可追溯。
3. WHEN 函数未被覆盖，系统 SHALL 输出与既有 `addNames.py` 完全一致的 `add_func` / `set_name` 行。
4. 系统 SHALL 在 `ida_script/` 目录输出追踪表 `semantic_names.txt`，每行格式 `{addr:#x} {原名} -> {语义名}`，供非 IDA 流程审计。

### R4 无退化保证

**User Story:** AS 维护者，I want 改动不破坏既有产物与回归基线，SO that 官方同步与持续回归不受影响。

#### Acceptance Criteria

1. WHEN 以 `NO_CODE_ANALYSIS` 编译（无代码分析数据），系统 SHALL 输出与改动前一致的 `addNames.py`。
2. WHEN 语义线索登记表为空，系统 SHALL 输出与改动前一致的 `addNames.py`。
3. 系统 SHALL 保证 SDK 库（url 含 `:`）全部函数命名与改动前逐字节一致。
4. 系统 SHALL 保证业务混淆库中未被覆盖函数的命名与改动前一致。
5. 回归断言 `scripts/regression.sh` 执行通过，PASS 计数不低于改动前。
