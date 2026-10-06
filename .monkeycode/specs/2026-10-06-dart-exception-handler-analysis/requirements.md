# 需求文档：Dart AOT 异常处理表解析与捕获边界标注

## Introduction

blutter 解析 Dart AOT 快照时忽略 `Code` 对象携带的异常处理元数据（`ExceptionHandlers` 与 `PcDescriptors` 的 try_index）。带 `try` / `catch` / `finally` 的函数在 `asm/*.dart` 与伪代码视图里只剩普通跳转，分析者无法判断哪段指令处于 try 保护区、catch 从句从哪条指令开始、捕获了哪些异常类型、以及 try 的嵌套关系。本功能解析异常表，并把 try / catch / finally 结构与捕获类型输出为可读视图。

## Glossary

- **try_index**：异常处理表的行号，同时标识一个 try 块；`PcDescriptors` 中每条记录携带其所属的 try_index。
- **ExceptionHandlers**：`dart::Code` 对象上的异常处理表，每行含 handler 起始 PC 偏移、外层 try_index、catch-all 标志与生成标志。
- **handled types**：某个 try_index 对应的 catch 从句捕获的类型列表，存于 `ExceptionHandlers.handled_types_data`。
- **catch entry moves**：`dart::Code.catch_entry_moves_maps()`，异常命中后运行时用于重排栈帧的搬运元数据。
- **handler PC 偏移**：相对 `Code.PayloadStart()` 的字节偏移（与 `PcDescriptors`、`catch_entry_moves_maps` 同一基准）。
- **target function**：blutter 中的一个 `DartFunction`。

## Requirements

### R1 异常表提取

**User Story:** AS 逆向分析者，I want 解析每个函数的异常处理元数据，so that 后续视图能展示异常结构。

#### Acceptance Criteria

1. WHEN 加载一个 `DartFunction` 且该函数 `Code` 的异常处理表非空，系统 SHALL 读取处理表行数与每行的 handler PC 偏移、外层 try_index、catch-all 标志、生成标志。
2. WHEN 加载一个 `DartFunction` 且某处理表行关联 catch 从句类型，系统 SHALL 读取该 try_index 的类型列表并保存每个类型的可读名称。
3. WHEN 加载一个 `DartFunction`，系统 SHALL 读取 `Code.pc_descriptors()` 并为每条记录保存其 PC 与 try_index。
4. WHEN 一个函数的 `Code` 为 UnknownDartCode 桩（混淆样本），系统 SHALL 为该函数保存空异常信息并继续解析其余函数。
5. WHEN 构建产物以 `NO_CODE_ANALYSIS` 编译，系统 SHALL 跳过异常表提取。

### R2 try 区域指令标注

**User Story:** AS 分析者，I want 汇编行显示其所属 try 区域，so that 一眼看出受保护的指令范围。

#### Acceptance Criteria

1. WHEN 命令行开启异常视图，且某条汇编指令的 PC 所属 try_index 大于等于 0，系统 SHALL 在该行注释中追加 `try<index>` 标记。
2. WHEN 命令行开启异常视图，且某条汇编指令的地址等于某个处理表行的 handler 起始地址，系统 SHALL 在该行注释中追加 `handler<index>` 起始标记。
3. WHEN 命令行开启异常视图，且某条汇编指令是某 try 区域的第一条或某 try 区域之后的第一条，系统 SHALL 在该行给出 try 区域开始或结束标记。

### R3 异常结构摘要与独立产物

**User Story:** AS 分析者，I want 函数级概览和独立异常表文件，so that 无需逐行扫描即可掌握异常结构。

#### Acceptance Criteria

1. WHEN 一个函数存在异常处理表，系统 SHALL 在函数头部输出以 `// exception:` 起头的摘要，逐行列出 try_index、handler 地址、捕获类型列表、catch-all 标志与生成标志。
2. WHEN 一个函数存在嵌套 try，系统 SHALL 在摘要中通过外层 try_index 标出 try 之间的父子关系。
3. WHEN 一次解析结束，系统 SHALL 在输出目录写出 `exceptions.txt`，按函数列出异常处理表与 try 区域范围。
4. WHEN 命令行开启异常视图，系统 SHALL 在 `asm/*.dart` 内输出函数级摘要与指令级标记；命令行关闭异常视图时，系统 SHALL 保持 `asm/*.dart` 与改动前逐字节一致。

### R4 无退化保证

**User Story:** AS 维护者，I want 新功能不影响既有产物，so that 官方同步与回归基线保持稳定。

#### Acceptance Criteria

1. WHEN 一个函数无异常处理表，系统 SHALL 输出与改动前一致的 `asm/*.dart`（在异常视图关闭时）。
2. WHEN `NO_CODE_ANALYSIS` 变体运行，系统 SHALL 输出与改动前一致的 `asm/*.dart`。
3. WHEN 读取异常元数据遇到越界或空指针，系统 SHALL 跳过该函数的异常信息并继续解析，且在标准错误输出一条警告。
