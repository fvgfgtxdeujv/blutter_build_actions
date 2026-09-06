# 实施任务：IDA 语义函数重命名闭环

- 日期：2026-09-03
- 对应：`requirements.md`（R1-R4）/ `design.md`
- 验证：arm64/x64 双二进制 ninja 编译 + `scripts/regression.sh` 全量回归（基线 PASS=53 → 61）

## 任务

- [x] 1. `DartDumper.h` 增加 `FnSemanticClues` 结构与 `fnSemanticClues_` 成员表（ep → strings/calls 桶）（R1；design Components）
- [x] 2. `DartDumper.cpp` 新增静态 helper：`isSdkLib`（url 含 `:`）、`isObfuscatedFnName`（`__unknown_function__`/`_ffi_resolver_function` 强命中 + 剥 `_` 核心 ≤4 且含大写/数字的短名命中）（R2 AC1-6；design 判定）
- [x] 3. `DartDumper.cpp` 新增 `genSemanticFnToken`：业务形字符串优先（`isBusinessToken`：小写开头/词形/3-28 长/`NAME_BLACKLIST`）、多候选取末尾、call 方法段兜底（calls 环同受黑名单与词形约束）（R1 AC4-7；design 候选生成；2026-09-03 两轮实测校准）
- [x] 4. `DartDumper.cpp` `DumpCode` 收集循环：命中 `isObfuscatedFnName` 且非 SDK 库的函数，把 pass1 字符串桶与 pass2/3 call 桶登记进 `fnSemanticClues_`（R1/R2；design Components）
- [x] 5. `DartDumper.cpp` `Dump4Ida` `dumpLib4Ida`：查 `fnSemanticClues_` 命中则 `set_name` 用 `fn_{token}` 覆盖、追加 `idaapi.set_cmt(ep, "origin: ...")`、另开流写 `semantic_names.txt`（R3 AC1-4；design Components/不变量 3-5）
- [x] 6. arm64/x64 双二进制 ninja 重编译 + zip/winapp 实测抽样：目检 `addNames.py` 的 `fn_` 命名、`set_cmt`、`semantic_names.txt` 与 SDK 库零覆盖（R4 AC1-2；design Test Strategy）
- [x] 7. `scripts/regression.sh` 新增 `check_semrename` 断言：`semantic_names.txt` 非空、`addNames.py` `::fn_` 行数与 `semantic_names.txt` 一致、SDK（dart 前缀库）零 `::fn_` 覆盖、既有黑名单断言保持通过（R4 AC3-5；design Test Strategy）
- [x] 8. 按两样本实际产物校准断言 min 值，全量回归 PASS=61 FAIL=0，提交推送（design Test Strategy 校准流程）
- [x] 9. 更新 `.monkeycode/docs/semantic-clue-collection.md`（补闭环产物说明）与 `.monkeycode/MEMORY.md`（记录命名覆盖范围约束与产物格式）
