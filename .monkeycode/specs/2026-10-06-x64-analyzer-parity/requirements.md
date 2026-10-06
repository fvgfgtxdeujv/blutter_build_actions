# 需求文档：x64 IL 分析器对齐 arm64

## Introduction

blutter 的 x64 IL 分析器（`blutter/src/CodeAnalyzer_x64.cpp`）是从 arm64 分析器移植而来，但两处关键能力尚未对齐：一是索引式参数槽读取（如 `[fp + rdx*4 + 0x18]`）未绑定到 IL 参数；二是可选参数与命名参数的 prologue 扫描为占位实现（`handleOptionalNamedParameters`，第 1627 行）。结果是 Flutter Windows 桌面 x64 `app.so` 的参数识别、变量命名与可选/命名参数还原弱于 Android arm64。本功能把 arm64 的对应分析补齐到 x64。

## Glossary

- **ArgumentsDescriptor**：Dart 调用约定中描述实参的元数据，含位置参数个数、命名参数名/位置对。
- **索引参数槽读取**：以 `[fp + index*4 + disp]` 形式沿实参向量遍历读取参数的指令模式。
- **固定参数（fixed parameter）**：出现在必需位置的参数。
- **可选参数（optional parameter）**：位置可选或命名可选的参数。
- **命名参数（named parameter）**：以名称匹配的可选参数，其名称按字典序排列。
- **prologue**：函数入口处保存实参、建立帧、登记参数变量的指令序列。

## Requirements

### R1 索引参数槽绑定

**User Story:** AS 分析者，I want x64 函数体中的索引参数读取绑定到具体参数，so that Windows 快照的变量命名与 arm64 一致。

#### Acceptance Criteria

1. WHEN 指令形如 `[fp + reg*4 + disp]` 且 `reg` 已被识别为当前命名参数索引，系统 SHALL 把该读取绑定到对应参数并记录参数变量。
2. WHEN 读取的索引寄存器来源于 ArgumentsDescriptor 的命名参数位置字段，系统 SHALL 把该读取绑定到对应命名参数。
3. WHEN 索引参数读取的基址寄存器不是帧指针，系统 SHALL 跳过该模式并保留原有处理。
4. WHEN IL 已把该读取识别为 `LoadValue`，系统 SHALL 复用现有 IL 结果，不重复登记参数。

### R2 固定参数 prologue 扫描

**User Story:** AS 分析者，I want x64 固定参数在 prologue 中被登记，so that 后续指令可引用参数变量。

#### Acceptance Criteria

1. WHEN prologue 按题设顺序保存固定参数到局部槽，系统 SHALL 为每个固定参数登记参数寄存器、局部偏移与参数序号。
2. WHEN 参数保存涉及寄存器间接寻址或扩展移位，系统 SHALL 依据 x64 寻址方式解析源寄存器与目标槽。
3. WHEN 函数参数个数由 `DartFunction::NumParam()` 给出，系统 SHALL 以该个数为扫描上界；参数个数未知时，系统 SHALL 持续扫描直到 prologue 模式不再匹配。

### R3 可选参数与命名参数 prologue 扫描

**User Story:** AS 分析者，I want x64 可选/命名参数被完整识别，so that 调用点参数与默认值可还原。

#### Acceptance Criteria

1. WHEN prologue 从 ArgumentsDescriptor 读取命名参数名称，系统 SHALL 将名称与函数签名的可选命名参数（按字典序）逐一匹配。
2. WHEN 某命名参数为 `required`，系统 SHALL 标记该参数为必需并在未匹配时继续扫描后续命名参数。
3. WHEN prologue 读取命名参数的名称或位置字段，系统 SHALL 依据 `AOT_ArgumentsDescriptor_name_offset` / `AOT_ArgumentsDescriptor_position_offset` 区分二者。
4. WHEN 扫描结束，系统 SHALL 清除为加载命名参数而引入的临时变量。
5. WHEN 一个可选位置参数由实参个数寄存器判定是否传入，系统 SHALL 通过 ArgumentsDescriptor 的实参个数确定可选参数数量。

### R4 与 arm64 分析器共享 IL 层

**User Story:** AS 维护者，I want 两架构尽量复用同一套 IL 与变量模型，so that 后续修改成本低。

#### Acceptance Criteria

1. WHEN x64 与 arm64 处理相同的参数语义，系统 SHALL 复用 `FnParams`、`VarParam`、`AnalyzingState`、`AnalyzingVars` 等共享结构。
2. WHEN x64 需要访问 ArgumentsDescriptor 常量，系统 SHALL 使用 Dart VM 头文件提供的 `AOT_ArgumentsDescriptor_*` 常量，不硬编码偏移。

### R5 无退化保证

**User Story:** AS 维护者，I want 改动不影响无可选/命名参数的函数，so that 既有产物稳定。

#### Acceptance Criteria

1. WHEN 一个函数没有可选参数与命名参数，系统 SHALL 输出与改动前一致的 IL 与 `asm/*.dart` 参数标注。
2. WHEN arm64 分析路径运行，系统 SHALL 输出与改动前一致的结果。
3. WHEN x64 遇到未识别的 prologue 模式，系统 SHALL 保留占位前的行为并继续分析后续指令。
