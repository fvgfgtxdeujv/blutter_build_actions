# 需求文档：Dart 类型与符号恢复增强

## Introduction

blutter 的类型与符号恢复存在若干已知缺口：内部类（`id <= kLastInternalOnlyCid`，如 `_Enum`、`_Type`）在类加载阶段提前返回，字段表与父类信息缺失，导致实例字段名与 enum 常量名恢复不完整；类层级只把变换混入类的最后一个接口当作 mixin，多接口/多 mixin 关系失真；泛型实参在子向量计算上对复杂类型可能错误；函数签名在多数函数中被丢弃，返回类型与参数类型为空。本功能在不改变默认输出的前提下补齐这些恢复能力。

## Glossary

- **内部类**：类 id 小于等于 `kLastInternalOnlyCid` 的 VM 内部类（如 `_Enum`、`_Type`）。
- **变换混入类**：编译器为 `with` 生成的 `_<child>&<extends>&<mixin>` 形态的合成类。
- **类型向量**：一个类的类型实参按父类链拼接成的向量；子类实参位于向量前段，父类实参位于后段。
- **函数签名**：`FunctionType`，含返回类型、参数类型、可选参数与命名参数布局。
- **字段镜像**：把实例字段的偏移映射到名称，用于 `objs.txt` / `pp.txt` 的字段名标注。

## Requirements

### R1 内部类字段镜像

**User Story:** AS 分析者，I want 内部类实例显示字段名，so that enum 常量与内部对象可读。

#### Acceptance Criteria

1. WHEN 加载一个内部类，系统 SHALL 尝试读取该类的字段表并在字段非空时建立偏移到名称的映射。
2. WHEN 一个内部类为无 Dart 字段的原生 C++ 类，系统 SHALL 记录该类为无字段并继续加载其余类。
3. WHEN 镜像 `_Enum` 实例，系统 SHALL 通过其 `_name` 字段输出 enum 常量名。
4. WHEN 某字段名为空，系统 SHALL 输出 `off_x: value` 形式而不崩溃。

### R2 类层级恢复

**User Story:** AS 分析者，I want 完整的继承/接口/mixin 关系，so that 方法归属与类型判断准确。

#### Acceptance Criteria

1. WHEN 加载一个类，系统 SHALL 记录其父类、全部直接接口与全部 mixin，而不是仅记录最后一个接口。
2. WHEN 一个类为变换混入类，系统 SHALL 从合成类名称与父类链还原真实的 `extends` 类与有序 mixin 列表。
3. WHEN 一个类的父类指针为 null 标记或非堆指针，系统 SHALL 记录为无父类并继续。
4. WHEN 类型是泛型，系统 SHALL 在以父类链计算类型向量时按各段类型参数个数切分，得到子类段与父类段。

### R3 泛型实参恢复

**User Story:** AS 分析者，I want 实例与函数显示泛型实参，so that 容器与泛型方法可读。

#### Acceptance Criteria

1. WHEN 一个类有类型参数，系统 SHALL 保存其类型参数名向量与父类类型参数子向量名。
2. WHEN 实例的类型实参来自对象头的类型实参槽，系统 SHALL 在输出中拼出 `<arg0, arg1, ...>`。
3. WHEN 分析器登记函数参数类型且该参数为泛型类，系统 SHALL 附带其类型实参（消除现有单参 `TODO`）。
4. WHEN 类型实参无法解析，系统 SHALL 输出不含实参的类名。

### R4 函数签名恢复

**User Story:** AS 分析者，I want 函数返回类型与参数类型，so that 调用语义可推断。

#### Acceptance Criteria

1. WHEN `Function.signature()` 为堆对象，系统 SHALL 读取返回类型与每个参数的类型、名称与必需标志。
2. WHEN `Function.signature()` 被丢弃，系统 SHALL 从 IL 分析得到的参数寄存器/槽类型回填返回类型与参数类型。
3. WHEN 函数为闭包，系统 SHALL 在父函数解析完成后回填该闭包的返回类型与参数类型。
4. WHEN 函数有类型参数，系统 SHALL 记录函数级类型参数名称列表。

### R5 无退化保证

**User Story:** AS 维护者，I want 恢复能力增强不改变默认反汇编，so that 回归基线稳定。

#### Acceptance Criteria

1. WHEN 字段或类型信息缺失，系统 SHALL 输出与改动前一致或不含该信息的占位，不产生崩溃。
2. WHEN 类型恢复失败，系统 SHALL 对实例输出 `UnhandledClass(name, cid=N)` 或原名，不中断解析。
3. WHEN `NO_CODE_ANALYSIS` 变体运行，系统 SHALL 输出与改动前一致的产物。
