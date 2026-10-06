# 需求文档：Dart 控制流结构化还原

## Introduction

blutter 的伪代码视图（`-p` / `--pseudo`）目前是线性指令流：寄存器折叠、字段/数组读写与调用点被还原为语句，但条件与无条件跳转只被标注成 `// if (...) goto 0x...` / `// goto 0x...`。分析者仍需自行在跳转目标之间建立基本块与循环、分支、switch 的结构。本功能在既有 IL 与汇编之上构建控制流图，并输出结构化的 `if/else`、`while/for`、`do/while`、`switch` 视图。

## Glossary

- **基本块（BasicBlock）**：一段顺序执行、只有一个入口和一个出口的指令序列。
- **CFG**：由基本块与有向边构成的控制流图。
- **回边（back edge）**：在 DFS 生成树上指向祖先块的有向边，标识一个自然循环。
- **汇集点（join point）**：多条前驱边汇合的块，是 if/else 的分支合流处。
- **后续块（successor）**：一条控制流边的目标块。
- **结构化视图**：把 CFG 还原成嵌套的 `if` / `while` / `for` / `switch` 语句文本。
- **不可归约（irreducible）**：无法用结构化语句完整表达的 CFG 区域。

## Requirements

### R1 基本块与 CFG 构建

**User Story:** AS 分析者，I want 由反汇编建立控制流图，so that 分支与循环关系可被自动分析。

#### Acceptance Criteria

1. WHEN 分析一个函数，系统 SHALL 以跳转目标、跳转指令的下一条指令以及函数入口作为基本块边界，切分该函数的全部汇编指令。
2. WHEN 一条指令是条件跳转、无条件跳转或返回，系统 SHALL 为该指令所在基本块登记到目标块或 fallthrough 块的边。
3. WHEN 一个跳转目标落在函数反汇编范围之外，系统 SHALL 把该目标登记为外部块并继续构建其余边。
4. WHEN 输入架构为 arm64 或 x64，系统 SHALL 依据对应架构的跳转助记符（`b` / `b.cond` / `cbz` / `tbz` 与 `jmp` / `jcc`）识别跳转。
5. WHEN IL 层已识别 `BranchIfSmi` 或 `CheckStackOverflow` 的跳转目标，系统 SHALL 采用 IL 提供的目标地址。

### R2 循环识别

**User Story:** AS 分析者，I want 自动识别循环，so that 伪代码显示 while/for 而非裸 goto。

#### Acceptance Criteria

1. WHEN 一条边从某块指向其在 DFS 生成树上的祖先块，系统 SHALL 把该边识别为回边并把该祖先块识别为循环头。
2. WHEN 已识别一个自然循环，系统 SHALL 计算该循环包含的基本块集合与唯一的循环尾。
3. WHEN 一个循环的循环头有循环外前驱，系统 SHALL 把该循环识别为可前置测试的 `while`；否则识别为 `do/while`。
4. WHEN CFG 含不可归约区域，系统 SHALL 对不可归约区域回退为带标签的 `goto` 输出。

### R3 分支与 switch 结构化

**User Story:** AS 分析者，I want if/else 与 switch 结构，so that 分支逻辑可读。

#### Acceptance Criteria

1. WHEN 条件跳转的两个后继在若干步内汇集到同一块，系统 SHALL 输出 `if (cond) { ... } else { ... }` 结构。
2. WHEN 条件跳转的两个后继中仅一个继续执行且另一个直接离开，系统 SHALL 输出无 `else` 的 `if` 结构。
3. WHEN 一个块的多条出边比较同一表达式并对不同常量取值分流，系统 SHALL 输出 `switch` 结构。
4. WHEN 一个条件分支的任一后继是循环头或循环体，系统 SHALL 把该分支与循环嵌套关系正确嵌套输出。

### R4 与既有伪代码视图集成

**User Story:** AS 维护者，I want 结构化视图复用现有折叠规则，so that 数据流还原不重复实现。

#### Acceptance Criteria

1. WHEN 命令行传入 `-p` / `--pseudo`，系统 SHALL 在既有折叠语句流的基础上输出结构化控制流。
2. WHEN 结构化构造某区域失败，系统 SHALL 对该区域回退为原有 `// if (...) goto 0x...` 标注行。
3. WHEN 未传入 `-p` / `--pseudo`，系统 SHALL 保持 `asm/*.dart` 与改动前逐字节一致。
4. WHEN 输出结构化视图，系统 SHALL 保留每条原始汇编行作为 `// 0x... <asm>` 参考行。

### R5 无退化保证

**User Story:** AS 维护者，I want 新分析不影响经典反汇编，so that 回归基线保持稳定。

#### Acceptance Criteria

1. WHEN 函数只含直线代码，系统 SHALL 输出与改动前一致的伪代码。
2. WHEN CFG 构建失败或超时，系统 SHALL 回退为线性伪代码并继续处理其余函数。
