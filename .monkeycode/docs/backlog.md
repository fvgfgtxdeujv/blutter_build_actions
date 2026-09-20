# 改进待办（Backlog）

记录时间：2026-09-17（2026-09-20 更新 C/D 落地并标注提交号）
分支基线：`master` `3596f9e`（A/B 见 `3596f9e`，C/D 见 `36b1217`，DartTypes/格式化修复见 `84d135a`）
用途：把「界定清晰、改动小」的候选改进集中登记，避免散落在代码 TODO 里。每项落地后请在此标注提交号，并同步更新 `README.md` / `semantic-clue-collection.md` 的相关段落。

## 0. 已排除（避免重复）

- 对象池 Field/Type/Function/非 stub Call 的语义线索扩收集：**已落地**，见 `semantic-clue-collection.md` 第 8 节（`DumpCode` 三趟 + 噪声黑名单）。
- 列表里的候选只登记**尚未实现**的部分。

## 1. 主线（重，暂缓）

### 控制流混淆重建

现状：`PseudoCode.cpp` 只把跳转输出为注释（`// goto 0x..`、`//<cond> goto 0x..`），见 `PseudoCode.cpp:691`、`:705`、`:1211`、`:1222`、`:1229`。没有基本块切分，也没有结构化控制流。

目标：从线性指令流重建 `if/else`、`while/for`、`switch`，并尝试还原控制流平坦化（dispatcher + state 变量）到原始基本块顺序。

为什么重：需要基本块切分（leader/jump target/jump 之后）、支配关系、循环识别（back edge）、dispatcher 模式匹配，且混淆后语义等价但顺序打乱，验证成本高。作为独立大项推进，先不动。

## 2. 低风险候选（推荐优先）

### A. 实例字段 offset→字段名

- 位置：`blutter/src/DartDumper.cpp:1408`（`dumpInstanceFields`），现有注释 `// TODO: match the offset to field name if possible`
- 现状：实例字段输出 `off_c: int(0x..)` / `off_10: Obj!...`
- 做法：用 `dartCls.Fields()`（`DartClass.h:54`）遍历非静态字段，按 `DartField::Offset()`（`DartField.h:22`）建 offset→`Name()` 映射，命中则输出 `name: value`，未命中保留 `off_x`
- 注意：混淆字段名可能是 `_adg` 这类短名，命中时建议同时保留 offset（如 `_adg(off_c): ...`）便于交叉核对
- 工作量：小（约 30-50 行）
- 验证：`scripts/regression.sh all` 仍 PASS=66；抽 `winapp`/`zip` 的 `pp.txt`、`objs.txt` 对比字段名命中数；asm 默认输出零漂移
- 状态：**已落地**（2026-09-17，提交 `3596f9e`）。实现：`dumpInstanceFields` 内用 `dartCls.Fields()` 建 offset→名映射（跳过 static 与空名），命中输出 `name (off_x): value`
- 落地实测：机制正确（`Symbol._name` 命中），但两个样本各只命中 4 处。原因已定位：混淆器把实例字段名抹成空串（`qea` offset 8 的字段 `Name()` 为空），而 `_Enum` 这类 VM 内部类在 `DartClass.cpp:37`（`id <= kLastInternalOnlyCid`）提前 return，字段表根本没加载。这是数据缺失，非映射 bug

### B. `ObjectToString` 兜底降级（消除 FATAL abort）

- 位置与现状：
  - `DartDumper.cpp:1117` `FATAL("TODO: simd array")`：typed array 元素为 `kFloat32x4ArrayElement` / `kInt32x4ArrayElement` / `kFloat64x2ArrayElement` 时整进程 abort
  - `DartDumper.cpp:1339` `FATAL("Unhandle internal class %s")`：未处理内部 cid 整进程 abort
- 做法：SIMD 数组元素按 dartvm 元素类型逐元素读取（同 `ACCUMLATE` 模板，标量分支已在 `:1098-1112`）；未知 cid 改为输出占位串（如 `UnhandledClass(cid)`）而非 abort
- 与 2026-09-17 修复同源：缺宏/未覆盖 cid 会让单个对象终止整个解析，健壮性问题应就地降级
- 工作量：小
- 验证：回归真输入仍 EXIT=0；构造/stub 覆盖到 FATAL 分支确认不再 abort
- 状态：**已落地**（2026-09-17，提交 `3596f9e`）。SIMD 三个 element 分支按 `dart::simd128_value_t` 联合体解码（float/int/double storage），未知内部 cid 改为返回 `UnhandledClass(name, cid=N)` 占位
- 落地实测：两样本池中无 SIMD 数组、无未处理 cid，故这两个分支未被真实触发（编译通过 + 逻辑对齐 `kSimd128Size = sizeof(simd128_value_t)`）；回归 PASS=66，asm 零漂移

### C. 实例显示补类名与库前缀

- 位置与现状：
  - `DartDumper.cpp:1330` `case dart::kInstanceCid` 输出 `Obj!Object@{:x}`
  - `DartDumper.cpp:1342` `// TODO: print library and package prefix`
  - `DartDumper.cpp:1358` `dumpInstance` 的 simpleForm 分支输出 `Obj!{dtype}@addr`
- 做法：`kInstanceCid` 走真实类名；`dtype` 输出用库前缀 + 具体类型实参（`[lib.url] Class<args>`；`FullNameWithPackage()` 会丢具体实参，故改为拼接而非直接调用）
- 工作量：很小
- 验证：`objs.txt` / 池描述抽样对比，断言无异常回退
- 状态：**已落地**（2026-09-20，提交 `36b1217`）。`kInstanceCid` 输出 `Obj![dart:core] Object@addr`；`dumpInstance` 的 simpleForm 与全形式类名统一为 `[lib.url] Class<args>`（`lib.url` 为空时不加前缀，兼容 native/dummy 类）
- 落地实测：两个样本 asm 池描述与 objs.txt 均带库前缀（如 `Obj![package:flutter/src/services/platform_channel.dart] Uea<Object?>@addr`）；归一化剥离 `[lib] ` 前缀后与基线逐字节一致

## 3. 中等候选

### D. enum 子类识别

- 位置：`DartDumper.cpp:1332` `// TODO: enum subclass`
- 现状：enum 实例走通用 `dumpInstance`
- 做法：判定类为 enum 后打印 `EnumName.value`（dartvm enum 相关 API 待核）
- 工作量：小-中
- 验证：抽含 enum 的样本对比输出
- 状态：**已落地**（2026-09-20，提交 `36b1217`）。`DartClass` 暴露 `Type()`；`dumpInstance` 命中 `ENUM` 时附加 `enumValueName()`：按 `_Enum` 布局扫描实例字段槽，取第一个 String 槽（即 `_name`）作为常量名，输出 `Obj![lib] EnumName.value@addr`，找不到名字则退回原样
- 落地实测：`_Enum` 为 VM 内部类，未镜像进 `DartClass` 字段名，但常量名可从实例内存取得；zip 得 `CSc.blockMappingStart`、`IPc.restore` 等，winapp 得 `FF.windows`、`ePb.file` 等（winapp 共 12661 处）。**顺带发现**：`walkObject`/`dumpInstanceFields` 对 unboxed 字段一律按 `kCompressedWordSize` 双字前进，在 x64 非压缩构建（kCompressedWordSize=8）会前进 16 字节、跳过其后的字段（如 `_Enum` 的 `_name` 在 0x10 处被跳过）；`enumValueName` 改用固定 8 字节前进以同时兼容 arm64/x64，未改动这两处既有逻辑

### E. IL 行补池对象描述

- 位置：`blutter/src/il.h:548` `// TODO: add pool object offset or pointer`
- 现状：IL 文本只有 `[pp+off]`；asm 路径已用 `getPoolObjectDescription` 附描述（`DartDumper.cpp` asm extra）
- 做法：IL 的 pool 引用行附同等描述（字符串/Field/Function 短名）
- 注意：`PseudoCode.h` 明确记录过——伪代码视图刻意不调用 `getPoolObjectDescription`，避免强制物化类型；此改动仅限 IL/asm 展示路径，勿动伪代码的惰性策略
- 工作量：小
- 验证：回归 PASS=66；伪代码默认输出零漂移

### F. 伪代码后向跳转识别为循环

- 位置：`blutter/src/PseudoCode.cpp:691`、`:705`（x64/通用）、`:1229`（arm64 无条件）
- 现状：所有跳转统一 `// goto 0x..` / `//<cond> goto 0x..`
- 做法：比较 target 与当前地址，target < addr 时改输出 `// while (...) goto 0x..`（循环回边），target > addr 保持 `// goto`；不引入 CFG，零结构风险
- 工作量：小
- 注意：伪代码为 `-p/--pseudo` 才输出，不影响默认 asm 零漂移
- 验证：`regression.sh all` PASS=66；抽样本确认回边行措辞

### G. x64 gap-4 参数槽

- 位置：`blutter/src/CodeAnalyzer_x64.cpp:17`（`TODO(gap-4)`）
- 内容：
  1. indexed arg-slot 读取 `[fp + rdx*4 + 0x18]`（args vector walk）尚未 IL 绑定，需确认 x64 ArgumentsDescriptor 槽位 ABI
  2. optional/named 参数 prologue 扫描为占位（`handleOptionalNamedParameters`）
- 背景：此前调研后回滚，标注 lower value / backlog
- 工作量：中（成本最高的一项）

## 4. 建议批次

1. 第一批：A + B（同在 `ObjectToString` 函数族，可读性 + 健壮性一次拿到，风险低、可断言）——**已落地**
2. 第二批：C + D + E（展示层小追加）——C + D **已落地**，E 待做
3. 伪代码主线：F → 之后视情况推进第 1 节控制流重建
4. G 视需求排期

## 5. 零漂移校验方法（C/D 后）

C/D 会**有意**改变实例描述（加 `[lib] ` 前缀、加 `.value` 后缀）。校验时先用 `python3 scripts/norm_diff.py <baseline_dir> <current_dir>` 归一化：剥离 `Obj![...] ` 前缀与 `Obj!Name.value@` 的 `.value`，再归一化 `0x`/`@` 地址与大十进制数，然后与改动前基线比对。zip/winapp 两样本均 drifted=0（仅还剩版本号等无关差异时需人工确认）。基线可先 `cp -r out_regress out_baseline_cd` 备份。
