# x64 IL 分析器移植：模式映射清单

> 目标：将 `CodeAnalyzer_arm64.cpp`（3734 行，25 个 matcher）的 IL 分析能力移植到 x64，
> 针对 Flutter Windows AOT `app.so`（Dart 3.3.4，precompiled 模式，Windows x64 ABI）。
> 本清单基于 Dart SDK 3.3.4 runtime 源码核对，供实现前确认。

## 1. 架构差异总览

| 维度 | arm64 | x64 (Windows AOT) |
|---|---|---|
| THR | X26 | R14 |
| PP | X27（tagged） | R15（tagged，`movq [R15+disp]` 时 disp = `element_offset - kHeapObjectTag`） |
| FP / SP | X29 / SP | RBP / RSP |
| TMP | X16 | R11（x64 无 TMP2） |
| CODE_REG | R12 | R12 |
| ARGS_DESC_REG | R4 | R10 |
| NULL_REG | R22 | 无（x64 无 NULL 寄存器，null 走对象池） |
| HEAP_BITS 寄存器 | X28（`add Xd,Xd,X28,LSL 32` 解压） | 无；解压用 `addq reg, [THR + heap_base_offset]` |
| Dart 函数调用 | 栈传参 + ArgumentsDescriptor | 栈传参 + ArgumentsDescriptor（`MakeCallSummary` 注释确认"all arguments pushed on the stack"），返回 RAX |
| 调用指令 | `bl`/`blr`/`b` | `call rel32` / `call [mem]` / `jmp rel32`（AOT 同 isolate 内 pc-relative call） |
| Smi | tag=0，1 bit，压缩指针下 kSmiBits=30 | 相同（Dart 3.3 x64 默认压缩指针，`DART_COMPRESSED_POINTERS`） |
| 堆对象 tag | kHeapObjectTag=1 | 相同 |
| true/false | 从 NULL 偏移加载（`add Xd, NULL, #0x20/0x30`） | 走对象池（x64 `LoadObjectHelper` 无 NULL 特例） |
| 对象池访问 | `ldr [PP,#disp]`；大偏移 `add+ldr`；MOVZ/MOVK 组合 | 仅 `movq dst,[PP+disp32]`（disp32 signed 覆盖整个池，无需多指令展开） |
| 写屏障 | `ldurb/and/tst/beq` + `bl stub` | `movb+shrl+andl+testb/jz` + `call rel32`（AOT 走 pc-relative wrapper stub） |
| 帧 | `stp fp,lr,[sp,#-16]!; mov fp,sp` | `pushq rbp; movq rbp, rsp`（AOT 不 push CODE/PP） |

## 2. 各 matcher 的 x64 指令模式

匹配顺序与 arm64 `matcherFns` 完全一致（顺序敏感，失败自动回退）。

### 2.1 processEnterFrameInstr → EnterFrameInstr
```
push rbp
mov rbp, rsp
```
- `X86_INS_PUSH` op[0].reg == RBP，随后 `X86_INS_MOV` (op[0]=RBP, op[1]=RSP)
- 设置 `fnInfo->useFramePointer = true`

### 2.2 processLeaveFrameInstr → LeaveFrameInstr
```
mov rsp, rbp
pop rbp
```
- `X86_INS_MOV` (op[0]=RSP, op[1]=RBP)，随后 consume `X86_INS_POP` op[0].reg==RBP
- 对应 arm64 `tryConsumeLeaveFrameRestore`

### 2.3 processAllocateStackInstr → AllocateStackInstr
```
sub rsp, imm
```
- `X86_INS_SUB` (op[0]=RSP, op[1]=RSP, op[2].imm)；stackSize = imm

### 2.4 processCheckStackOverflowInstr → CheckStackOverflowInstr
```
cmp rsp, [r14 + stack_limit_offset]
jbe <slow_path>        ; X86_INS_JBE, op[0].imm
```
- 相比 arm64（`ldr tmp; cmp sp,tmp; b.ls`）更短：直接 `cmp rsp,[THR+off]`
- slow_path 在函数尾部（`jbe` 目标 > 当前地址 < AddressEnd），记录 `firstCheckStackOverflowAddr`

### 2.5 processCallLeafRuntime → CallLeafRuntimeInstr
- arm64 核心模式：`mov sp,dsp` + `ldr TMP,[THR+off]` + `str TMP,[THR+vm_tag]` + `blr TMP` + 恢复 vm_tag + 检查 saved_stack_limit + `sub sp,tmp,#1<<12` + leave frame
- x64 对应（CCallInstr / LeafRuntimeScope）：`mov rsp, rbp`（先 leave frame 到调用现场）→ `mov TMP, [THR + call_native_through_safepoint_entry_point_offset]` → `call TMP`/`call [THR+off]` 或 `mov qword [THR+vm_tag], imm` 等
- **x64 具体形态需用真实 app.so 校准**（本清单按 `LeafRuntimeScope::Call` + x64 `CallRuntime` 推测：`sub rsp, shadow` → `call [THR+entry]`；见风险 6.2）

### 2.6 processObjectPoolInstr → LoadValueInstr / StoreObjectPoolInstr
```
movq reg, [r15 + disp]      ; 加载，offset = disp
movq [r15 + disp], reg      ; 存储（isWrite），offset = disp
```
- x64 无 P2（add+ldr 大偏移）、无 P3（NULL true/false）、无 MOVZ/MOVK 池偏移
- `getPoolObject` 逻辑（IndexFromOffset / TypeAt / 各 kXxxCid 分支）完全复用 arm64（架构中立）
- 压缩指针下对象池存完整 8 字节指针，movq 直接可用，**无需解压**

### 2.7 processLoadValueNoObjectPoolInstr → LoadValueInstr（立即数）
```
mov reg, imm                 ; X86_INS_MOV op[1].imm，int32/imm64
xor reg, reg                 ; 0
mov reg, 1                   ; mov reg,1 (也可被上条覆盖)
mov reg, -1                  ; mov reg, -1
```
- arm64 MOVZ/MOVK/ORR/MOVN/EOR 组合在 x64 都是单条 `mov reg, imm64`
- `xor reg,reg` → VarInteger 0

### 2.8 processDecompressPointerInstr → DecompressPointerInstr
```
addq reg, [r14 + heap_base_offset]
```
- 对应 arm64 `add Xd, Xd, X28, LSL 32`；x64 无 HEAP_BITS 寄存器，从 THR 加载 heap_base
- 出现于 LoadCompressed（`movl dst,[base+off]` 之后）与独立解压场景

### 2.9 processClosureCallInstr → ClosureCallInstr
- arm64 模式：`ldur x2,[x0,closure_entry_point]`（前一条是加载 ARGS_DESC 的 LoadValue）→ `blr x2`
- x64 对应（ClosureCallInstr::EmitNativeCode）：
```
movq reg, [rax + closure_entry_point_offset - kHeapObjectTag]  ; 或从 pool 加载 entry
call reg    ; call [reg+...] 形式
```
- 前置：`movq r10, [pool]` 加载 ArgumentsDescriptor（ARGS_DESC_REG）
- 需解析 ArgumentsDescriptor（数组元素：typeArgsLen/argCnt/argSize/positionalCnt/names）——复用 arm64 逻辑

### 2.10 processSaveRegisterInstr → SaveRegisterInstr
```
push reg      ; X86_INS_PUSH
```
### 2.11 processLoadSavedRegisterInstr → RestoreRegisterInstr
```
pop reg       ; X86_INS_POP
```

### 2.12 processInitAsyncInstr → InitAsyncInstr
- arm64：`bl <InitAsyncStub>`（pc-relative），前置 LoadValue（R0 = type args / null）
- x64：`call rel32`（InitAsyncStub）前置 `movq rax, [PP+off]`（LoadValue R0）；stub 名称通过 `app.GetFunction(call_imm)` 判定 `IsStub() && kind == InitAsyncStub`

### 2.13 processCallInstr → CallInstr（pc-relative / tail）
```
call rel32         ; X86_INS_CALL, op[0].imm == 目标绝对地址
jmp rel32          ; X86_INS_JMP, op[0].imm，目标在函数外 → tail call
```
- 与 arm64 `bl`/`b` 语义相同，`app.GetFunction(target)` 解析

### 2.14 processGdtCallInstr → GdtCallInstr（dispatch table call）
```
movq rax, [r14 + dispatch_table_array_offset]   ; LoadDispatchTable
call qword ptr [rax + rcx*8 + disp]             ; X86_INS_CALL op[0].mem
```
- 匹配：前一条 `movq RAX,[THR+dispatch_table_array_offset]`，本条 `call mem{base=RAX, index=RCX, scale=8}`
- offset = `disp / 8`（arm64 提取的是 `selector_offset - kOriginElement`，x64 的 disp 已乘 8；语义一致）
- `GdtCallInstr(offset)`

### 2.15 processReturnInstr → ReturnInstr
```
ret    ; X86_INS_RET
```
- （可选识别 `mov rsp, rbp; pop rbp; ret` 前的 LeaveFrame，但 LeaveFrame 已在 2.2 独立处理）

### 2.16 processInstanceofNoTypeArgumentInstr → TestTypeInstr
- arm64 模式：`mov RAX(src)`+`mov RDX,NULL`+`mov RCX,NULL` → BranchIfSmi → LoadClassId → `sub/sub/cmp/b.ls`（int/num 快检）→ 从 PP 加载 dstType(RBX) → 加载 STC(R9) 或 `bl TypeTestStub`
- x64 对应（GenerateInlineInstanceof + GenerateInstantiatedTypeNoArgumentsTest + GenerateSubtype1TestCacheLookup）：
```
mov rax, src            ; TypeTestABI::kInstanceReg = RAX
mov rdx, 0 / xor rdx    ; kInstantiatorTypeArgumentsReg = RDX
mov rcx, 0 / xor rcx    ; kFunctionTypeArgumentsReg = RCX
test rax, 1; je <done>               ; BranchIfSmi（X86_INS_TEST + X86_INS_JE）
movl tmp, [rax + tags_offset - 1]; shrl tmp, 12   ; LoadClassId（kScratchReg=RSI）
sub tmp, kSmiCid
cmp tmp, 1(或2)
jbe <done>
movq rbx, [r15 + disp]               ; kDstTypeReg = RBX（VarType）
movq r9, [r15 + disp]                ; kSubtypeTestCacheReg = R9（kSubtypeTestCacheCid）
call rel32                            ; TypeTestStub / 或 call [rbx + type_test_stub_entry_point_offset]
```
- **需对照 x64 实际 stub 调用形态校准**（见风险 6.2）：可能 `call [rax]`（STC 结果）或 `call rel32`

### 2.17 processBranchIfSmiInstr → BranchIfSmiInstr
```
test reg, 1      ; X86_INS_TEST, op[1].imm == kSmiTagMask(1)
je <branch>      ; X86_INS_JE（Smi 时 ZF=1）
```
- 与 arm64 `tbz reg,#0,target` 等价；分支目标为 `je` 的 imm

### 2.18 processLoadClassIdInstr → LoadClassIdInstr
```
movl reg, [obj + tags_offset - kHeapObjectTag]   ; X86_INS_MOV 32位
shrl reg, 12                                      ; X86_INS_SHR, imm=12
```
- `kUntaggedObjectClassIdTagPos == 12`（与 arm64 同常量）；`movl` 天然零扩展高位，无需 arm64 的 UBFX

### 2.19 processBoxInt64Instr → BoxInt64Instr
- arm64 模式：`sbfiz x0,x2,#1,#31` → `cmp x2,x0,asr#1` → `b.eq cont` → `bl AllocateMint*Stub` → `stur x2,[x0,#7]`
- x64 压缩指针模式（il_x64.cc BoxInt64Instr::EmitNativeCode）：
```
leaq out, [value + value]        ; out = value*2（Smi tag；X86_INS_LEA base=value,index=value,scale=1）
; 若 ValueFitsSmi → 结束
movq temp, value
sarq temp, 30
addq temp, 1
cmpq temp, 2
jb <done>
call rel32                        ; AllocateMintShared*Stub（或 pool→CODE→call）
movq [out + Mint::value_offset - 1], value
<done>:
```
- 识别 leaq [v+v] + sar 30 + cmp/jb 序列；stub 名称校验 `AllocateMintSharedWithout/WithFPURegsStub`

### 2.20 processLoadInt32FromBoxOrSmiInstr → LoadInt32Instr
- arm64 模式：`sbfx out,in,#1,#31` (+ `tbz in,#0` → `ldur out,[in,mint_value]`)
- x64 压缩指针模式（LoadInt32FromBoxOrSmi）：
```
sarq out, 1          ; SmiUntagAndSignExtend（X86_INS_SAR）
jnc <done>           ; X86_INS_JAE（原值 LSB=1 是 Smi，sar 后 CF=1？需校准条件）
; 非 Smi → Mint：
movsxd/movq out, [in + Mint::value_offset - 1]
<done>:
```
- **条件码校准见风险 6.2**（arm64 是 `tbz` 正向，x64 用 CF/`jae`，需按实际反汇编核对）

### 2.21 processLoadTaggedClassIdMayBeSmiInstr → LoadTaggedClassIdMayBeSmiInstr
- 聚合 IL：`movq cid, Smi(kSmiCid)` → `test obj,1; je done` → `movl cid,[obj+tags-1]; shrl cid,12` → `shl cid,1`(SmiTag) → `done`
- arm64 matcher 从已生成的 LoadValue+BranchIfSmi+LoadClassId+LSL 回溯聚合，x64 同样回溯（il_insns 尾部 3-4 条）

### 2.22 processLoadFieldTableInstr → LoadStaticFieldInstr / StoreStaticFieldInstr / InitLateStaticFieldInstr
```
movq reg, [r14 + field_table_values_offset]   ; Thread::field_table_values
movq reg, [reg + Smi(field_offset)]           ; load（Smi 字段偏移 = field_offset<<1）
movq [reg + Smi(field_offset)], reg           ; store
```
- arm64 大偏移 `add` 步骤在 x64 由 disp32 直接承载（无额外指令）
- late 初始化检查：随后 `movq tmp,[PP+disp]`（Sentinel）→ `cmp dst,tmp` → `jne <cont>` → 加载 Field 到 RDX（InitStaticFieldABI）→ `call rel32`（InitLateStaticFieldStub）

### 2.23 processTryAllocateObject → AllocateObjectInstr（内联分配）
```
movq inst, [r14 + top_offset]
addq inst, instance_size
cmpq inst, [r14 + end_offset]
jae <slow_path>
movq [r14 + top_offset], inst
addq inst, kHeapObjectTag - instance_size     ; 得到 tagged 指针
movq [inst + tags_offset - 1], tags_imm        ; MoveImmediate 写 class tags
```
- 对应 arm64 `ldp/add/cmp/b.ls/str/sub/movz/movk/stur`；cid 从 tags_imm 提取（`>> kClassIdTagPos`）

### 2.24 processWriteBarrierInstr → WriteBarrierInstr
```
; kValueCanBeSmi 时：
test reg_val, 1
je <done>
movb tmp, [obj + tags_offset - 1]             ; movb 8位（TMP=R11 的 byte）
shrl tmp, kBarrierOverlapShift
andl tmp, [r14 + write_barrier_mask_offset]
testb [val + tags_offset - 1], tmp
jz <done>
call rel32          ; write_barrier_wrappers_stub（或 array_write_barrier_stub），AOT pc-relative
<done>:
```
- x64 无固定 WB_OBJECT/WB_VALUE 寄存器约定（区别于 arm64），obj/val 由指令操作数直接识别
- isArray 判定：call 目标 stub 名称含 array 或 offsets 数组命中
- 对应 `generate_invoke_write_barrier_wrapper_`（AOT 下 `GenerateUnRelocatedPcRelativeCall` → `call rel32`）
- **慢路径回退形态需校准**（pool 加载 + call [reg]）

### 2.25 processLoadStore → Load/StoreArrayElementInstr / LoadFieldInstr / StoreFieldInstr
- **带索引数组访问**（index 为寄存器）：
```
add tmp, arr, idx(, scale)          ; 计算元素地址
mov X, [tmp + data_offset - tag]    ; LoadIndexed（mov/movsx/movzx/mov 各宽度）
mov [tmp + data_offset - tag], X    ; StoreIndexed
```
- **固定偏移对象/数组访问**：
```
movl/movq val, [obj + disp]         ; LoadField（压缩指针下随后跟 addq [THR+heap_base]）
movq [obj + disp], val              ; StoreField（随后可能跟写屏障 2.24）
```
- arm64 的 `getArrayOp` 按指令宽度/符号区分 typed array 类型；x64 用 `op.size`（1/2/4/8）+ 符号扩展指令（MOVSX/MOVZX）映射，List 元素 = 4 字节压缩
- `movb/movw/movl/movq [obj+disp], val` + 写屏障判定数组/对象（复用 arm64 逻辑：有 array 写屏障→StoreArrayElement，否则 StoreField）

## 3. 参数处理（prologue 参数识别）

- arm64 分析器在 `handlePrologue` 中经 `processPrologueParametersInstr` → `handleFixedParameters` / `handleOptionalPositionalParameters` / `handleOptionalNamedParameters` / `handleArgumentsDescriptorTypeArguments` / `handleParameterRegisters` 识别参数。
- **x64 AOT 下 Dart 函数参数全部位于调用者栈**（与 arm64 相同），分析要点：
  - 固定参数：直接 `mov valReg, [rbp + (i+2)*8]`（+8 return addr +8 saved rbp；arm64 用动态 `add Xn,FP,Xcnt,SXTW#2` 因为参数可能跨寄存器/栈，x64 全部栈上则编译器直接编址）
  - 可选位置参数：`[rbp+disp]` 固定偏移或经 ArgumentsDescriptor 动态索引（arm64 有参数计数寄存器计算，x64 对应形态待样本校准）
  - 命名参数：从 ArgumentsDescriptor 读取（`mov reg,[rbp+?]` → 解析 name/count）
  - 泛型类型参数：ArgumentsDescriptor 的 type args 区
- `AnalyzingState`/`AnalyzingVars`/`FnParams` 完全架构中立，直接复用；仅 A64::Register 换 x64 别名（`kNumberOfRegisters=16` 一致）

## 4. convertAsm / asm2il 主循环

- `asm2il`：`handlePrologue` + matcher 顺序匹配 + `UnknownInstr` 兜底 —— 逻辑与 arm64 完全相同，仅 matcher 内部指令判断换成 x64
- `convertAsm`：保留对方 MVP 的寄存器重命名（r14→THR、r15→PP、r12→CODE、r11→TMP、r10→ARGS_DESC、rbp→fp、rsp→SP）与 PoolOffset/ThreadOffset/Call 标注；新增：
  - PoolOffset：disp 语义已正确（`element_offset - kHeapObjectTag`，MVP 注释已确认）
  - Call 目标解析：`call/jmp rel32` 的 imm；`call [rax+rcx*8+disp]` 标 GDT call
  - `first_stack_limit_addr`：`cmp rsp,[r14+stack_limit]` 处标记

## 5. 常量与偏移核对（已确认）

- `kSmiTag=0, kHeapObjectTag=1, kSmiTagMask=1, kSmiTagShift=1`（pointer_tagging.h，arm64/x64 相同）
- `kSmiBits=30`（压缩指针下，globals.h）
- `kUntaggedObjectClassIdTagPos=12, kClassIdTagSize=20`（x64 LoadClassId 断言 12/20，与 arm64 相同）
- 帧偏移：`[rbp+8]=return addr`，参数从 `[rbp+16]` 起
- `Thread::*` 偏移名均已在 `DartThreadInfo.cpp` 注册：`stack_limit, saved_stack_limit, write_barrier_mask, heap_base, field_table_values, top, end, dispatch_table_array`
- 调用约定（Windows x64 AOT）：`kArg1Reg=RCX, kArg2Reg=RDX, kArg3Reg=R8, kArg4Reg=R9`（C 调用），返回 RAX；Dart 函数间调用栈传参
- TypeTestABI：`kInstanceReg=RAX, kDstTypeReg=RBX, kInstantiatorTypeArgumentsReg=RDX, kFunctionTypeArgumentsReg=RCX, kSubtypeTestCacheReg=R9, kScratchReg=RSI`；DispatchTableNullErrorABI::kClassIdReg=RCX
- InitStaticFieldABI::kFieldReg=RDX、kResultReg=RAX；LateInitializationErrorABI::kFieldReg=RSI
- WriteBarrier：`kWriteBarrierObjectReg=RDX, kWriteBarrierValueReg=RAX, kWriteBarrierSlotReg=R13`

## 6. 风险与开放问题

1. **无法本地编译**：x64 分支仅在 Windows CI 编译，语法/类型错误按 CI 报错迭代修复。
2. **需真实 app.so 校准的模式**（本清单基于 SDK 源码推断，最终以 Flutter Windows `app.so` 反汇编为准）：
   - 2.5 CallLeafRuntime、2.16 InstanceOf 的 stub 调用形态（`call rel32` vs `call [reg+entry_point]`）
   - 2.20 LoadInt32FromBoxOrSmi 的条件码方向
   - 2.24 写屏障 wrapper 调用（pc-relative vs 对象池）
   - 3. 可选/命名参数在 x64 AOT 的栈寻址形态
   - 2.9 ClosureCall 的 entry_point 加载方式
3. **x64 `TMP2 = kNoRegister`**：arm64 matcher 中 TMP2 的使用点（如 GDT 的 `movz/movk` 中间值、写屏障 val 寄存器）在 x64 无对应，需按 x64 实际指令重写这些子模式。
4. **movl 零扩展 vs movq**：capstone 中同是 `X86_INS_MOV`，需靠 `op.size`（4 vs 8）区分压缩指针字段加载与完整指针加载。
5. **jcc 识别**：x64 用独立 insn id（X86_INS_JAE/JBE/JE/JNE/JB...），arm64 的 `insn.cc()` 不适用，matcher 直接判 id。capstone 无 `X86_INS_JC/JNC/JZ/JNZ`，统一为 `JB/JAE/JE/JNE`。

## 7. 实现修正记录（2026-08-15，CodeAnalyzer_x64.cpp 已实现）

### 7.1 非压缩指针（关键修正）
`scripts/build.py` 中 `windows_x64` 配置 `compressed_ptrs: False`，Flutter Windows 桌面 AOT 默认**关闭压缩指针**（Dart 对 Windows 目标的默认行为）。因此：
- **无 DecompressPointer**：x64 非压缩下没有 `addq reg,[THR+heap_base]` 解压指令（matcher 保留但不匹配）
- **Smi 为 62 位**：`kSmiBits = kBitsPerWord - 2`
- **List 元素 8 字节**（非压缩完整指针）
- **BoxInt64**（非压缩）：`mov out,value; addq out,out; jno done; call <AllocateMint*Stub>; movq [out+Mint_value-tag],value`
  - `SmiTag = addq reg,reg`（非 leaq），溢出检测用 `X86_INS_JNO`（OF=0 即 Smi 内）
- **LoadInt32FromBoxOrSmi**（非压缩）：`sarq out,1; jae done; movsxd out,[out+out+Mint_value-tag]`
  - Mint 加载是 `[out*2 + offset]`（Address(out,out,TIMES_1)），符号扩展用 `X86_INS_MOVSXD`

### 7.2 capstone 指令枚举核对
从 capstone `include/capstone/x86.h` 确认：
- jcc 无 `X86_INS_JC/JNC/JZ/JNZ`，分别用 `X86_INS_JB/JAE/JE/JNE`
- `X86_INS_MOVABS`（movabs 大立即数）需与 `X86_INS_MOV` 一并处理
- `X86_INS_MOVSX`/`X86_INS_MOVSXD` 用于带符号扩展的 typed array 元素加载

### 7.3 已实现 matcher 与关键决策
- 25 个 matcher 全部实现于 `CodeAnalyzer_x64.cpp`（约 1850 行）
- 对象池：仅 `movq [R15+disp32]`（PP tagged，disp = element_offset - 1）
- 帧：`push rbp; mov rbp,rsp`；leave：`mov rsp,rbp; pop rbp`
- 栈溢出检查：`cmp rsp,[R14+stack_limit]; jbe slow`
- GDT call：`mov rax,[R14+dispatch_table_array]; call [rax+rcx*8+disp]`，offset = disp/8
- ClosureCall：`mov rcx,[rax+Closure_entry_point-tag]; call rcx`（前置 LoadValue ARGS_DESC_R10）
- 写屏障：`test val,1; je; movb tmp,[obj+tags]; shrl; andl [THR+mask]; testb [val+tags]; jz; call rel32`
- 参数处理：固定参数从 `[rbp+8*(i+2)]` 读取；可选参数/命名参数/type args 为镜像简化版，标注待样本校准
- convertAsm 保留对方 MVP 的寄存器重命名 + PoolOffset/ThreadOffset/Call 标注
