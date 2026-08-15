#include "pch.h"
#include "CodeAnalyzer.h"
#include "DartApp.h"
#include "VarValue.h"
#include "DartThreadInfo.h"
#include <source_location>
#include <unordered_set>

#ifndef NO_CODE_ANALYSIS

// ============================================================================
// x64 IL analyzer port from CodeAnalyzer_arm64.cpp
// Target: Flutter Windows AOT app.so (Dart 3.3.x, precompiled, x86-64)
// IL layer (il.h / VarValue.h / CodeAnalyzer.h) is shared and arch-neutral;
// A64::Register is an x64 register alias under the A64 namespace.
// ============================================================================

// auto revert ASM iterator to the current when pattern does not match or an exception occurs
class InsnMarker
{
public:
	explicit InsnMarker(AsmIterator& insn) : insn(insn), mark(insn.Current()) {}
	~InsnMarker() {
		if (mark)
			insn.SetCurrent(mark);
	}

	int64_t Take() {
		auto res = mark->address;
		mark = nullptr;
		return res;
	}

	cs_insn* Insn() {
		return mark;
	}

private:
	AsmIterator& insn;
	cs_insn* mark;
};

class InsnException
{
public:
	explicit InsnException(const char* cond, AsmIterator& insn, const std::source_location& location = std::source_location::current())
		: cond{ cond }, insn{ insn.Current() }, location{ location } {}

	std::string cond;
	cs_insn* insn;
	std::source_location location;
};

#define INSN_ASSERT(cond) \
  do {                    \
	if (!(cond)) throw InsnException(#cond, insn); \
  } while (false)

static bool IsX86Imm(const cs_x86_op& op) { return op.type == X86_OP_IMM; }
static bool IsX86Reg(const cs_x86_op& op) { return op.type == X86_OP_REG; }
static bool IsX86Mem(const cs_x86_op& op) { return op.type == X86_OP_MEM; }
static bool IsX86MemBase(const cs_x86_op& op, x86_reg base) { return IsX86Mem(op) && (x86_reg)op.mem.base == base; }
static bool IsX86MemDisp(const cs_x86_op& op, x86_reg base, int64_t disp) {
	return IsX86Mem(op) && (x86_reg)op.mem.base == base && op.mem.disp == disp;
}

// ----------------------------------------------------------------------------
// Object pool -> VarValue (arch-neutral, ported verbatim from arm64)
// ----------------------------------------------------------------------------
static VarValue* getPoolObject(DartApp& app, intptr_t offset, A64::Register dstReg)
{
	intptr_t idx = dart::ObjectPool::IndexFromOffset(offset);
	auto& pool = app.GetObjectPool();
	auto objType = pool.TypeAt(idx);
	if (objType == dart::ObjectPool::EntryType::kTaggedObject) {
		auto ptr = pool.ObjectAt(idx);
		// Smi is special case. Have to handle first
		if (!ptr.IsHeapObject()) {
			return new VarInteger(dart::RawSmiValue(dart::Smi::RawCast(ptr)), dart::kSmiCid);
		}

		if (ptr.IsRawNull())
			return new VarNull();

		auto& obj = dart::Object::Handle(ptr);
		if (obj.IsString())
			return new VarString(dart::String::Cast(obj).ToCString());

		if (obj.IsTypedData()) {
			return new VarExpression(std::format("{}", obj.ToCString()), (int32_t)obj.GetClassId());
		}

		switch (obj.GetClassId()) {
		case dart::kSmiCid:
			return new VarInteger(dart::Smi::Cast(obj).Value(), dart::kSmiCid);
		case dart::kMintCid:
			return new VarInteger(MintValue(dart::Mint::Cast(obj)), dart::kMintCid);
		case dart::kDoubleCid:
			return new VarDouble(dart::Double::Cast(obj).value());
		case dart::kBoolCid:
			return new VarBoolean(dart::Bool::Cast(obj).value());
		case dart::kNullCid:
			return new VarNull();
		case dart::kCodeCid: {
			const auto& code = dart::Code::Cast(obj);
			auto stub = app.GetFunction(code.EntryPoint() - app.base());
			ASSERT(stub);
			return new VarFunctionCode(*stub);
		}
		case dart::kFieldCid: {
			const auto& field = dart::Field::Cast(obj);
			auto dartCls = app.GetClass(field.Owner().untag()->id());
			auto dartField = dartCls->FindField(field.TargetOffset());
			ASSERT(dartField);
			return new VarField(*dartField);
		}
		case dart::kArrayCid:
		case dart::kImmutableArrayCid:
			return new VarArray(dart::Array::Cast(obj).ptr());
		case dart::kFunctionCid:
		case dart::kClosureCid:
		case dart::kConstMapCid:
		case dart::kConstSetCid:
			return new VarExpression(std::format("{}", obj.ToCString()), (int32_t)obj.GetClassId());
#ifdef HAS_RECORD_TYPE
		case dart::kRecordCid: {
			return new VarExpression(std::format("{}", obj.ToCString()), (int32_t)obj.GetClassId());
		}
#endif
		case dart::kTypeParametersCid:
			throw std::runtime_error("Type parameter in Object Pool");
		case dart::kTypeCid:
			return new VarType(*app.TypeDb()->FindOrAdd(dart::Type::Cast(obj).ptr()));
#ifdef HAS_RECORD_TYPE
		case dart::kRecordTypeCid:
			return new VarRecordType(*app.TypeDb()->FindOrAdd(dart::RecordType::Cast(obj).ptr()));
#endif
		case dart::kTypeParameterCid:
			return new VarTypeParameter(*app.TypeDb()->FindOrAdd(dart::TypeParameter::Cast(obj).ptr()));
		case dart::kFunctionTypeCid:
			return new VarFunctionType(*app.TypeDb()->FindOrAdd(dart::FunctionType::Cast(obj).ptr()));
		case dart::kTypeArgumentsCid: {
			return new VarTypeArgument(*app.TypeDb()->FindOrAdd(dart::TypeArguments::Cast(obj).ptr()));
		}
		case dart::kSentinelCid:
			return new VarSentinel();
		case dart::kUnlinkedCallCid: {
			intptr_t idx = dart::ObjectPool::IndexFromOffset(offset + 8);
			ASSERT(pool.TypeAt(idx) == dart::ObjectPool::EntryType::kImmediate);
			auto imm = pool.RawValueAt(idx);
			auto dartFn = app.GetFunction(imm - app.base());
			return new VarUnlinkedCall(*dartFn->AsStub());
		}
		case dart::kSubtypeTestCacheCid:
			return new VarSubtypeTestCache();
		case dart::kInt32x4Cid:
		case dart::kFloat32x4Cid:
		case dart::kFloat64x2Cid:
			return new VarExpression(std::format("{}", obj.ToCString()), (int32_t)obj.GetClassId());
		case dart::kLibraryPrefixCid:
		case dart::kInstanceCid:
			return new VarInstance(app.GetClass(dart::kInstanceCid));
		}

		if (obj.IsInstance()) {
			auto dartCls = app.GetClass(obj.GetClassId());
			if (dartCls->Id() < dart::kNumPredefinedCids) {
				std::cerr << std::format("Unhandle predefined class {} ({})\n", dartCls->Name(), dartCls->Id());
			}
			return new VarInstance(dartCls);
		}

		throw std::runtime_error("unhandle object class in getPoolObject");
	}
	else if (objType == dart::ObjectPool::EntryType::kImmediate) {
		auto imm = pool.RawValueAt(idx);
		if (dstReg.IsDecimal())
			return new VarDouble(*((double*)&imm), VarType::NativeDouble);
		return new VarInteger(imm, VarValue::NativeInt);
	}
	else if (objType == dart::ObjectPool::EntryType::kNativeFunction) {
		throw std::runtime_error("getting native function pool object from Dart code");
	}
	else {
		throw std::runtime_error(std::format("unknown pool object type: {}", (int)objType).c_str());
	}
}

// ----------------------------------------------------------------------------
// x64 AsmIterator over capstone x86-64 instructions
// ----------------------------------------------------------------------------
class AsmIterator {
	cs_insn* insnStart;
	cs_insn* insnEnd;
	cs_insn* insn; // current instruction
	cs_insn dummyInsnEnd;
public:
	AsmIterator(cs_insn* start, cs_insn* end) : insnStart(start), insnEnd(end), insn(insnStart) {
		dummyInsnEnd.id = 0;
		dummyInsnEnd.address = insnEnd->address + insnEnd->size;
		dummyInsnEnd.size = 0;
	}

	cs_insn* Current() { return insn; }
	void SetCurrent(cs_insn* ins) { insn = ins; }
	// prefix increment
	AsmIterator& operator++() {
		ASSERT(insn != &dummyInsnEnd);
		if (insn == insnEnd) {
			insn = &dummyInsnEnd;
		}
		else {
			++insn;
			while (insn != insnEnd && insn->id == X86_INS_NOP) {
				++insn;
			}
			if (insn == insnEnd)
				insn = &dummyInsnEnd;
		}
		return *this;
	}
	AsmIterator& operator--() {
		--insn;
		return *this;
	}
	AddrRange Wrap(int64_t start) {
		return AddrRange(start, insn->address);
	}
	bool IsEnd() {
		return insn == &dummyInsnEnd;
	}

	// jump instructions: return the branch target (absolute) if the current
	// instruction is a Jcc with an immediate target, else 0.
	uint64_t BranchTarget() const {
		if (!insn->detail || insn->detail->x86.op_count == 0)
			return 0;
		const auto& op = insn->detail->x86.operands[0];
		if (op.type != X86_OP_IMM)
			return 0;
		return (uint64_t)op.imm;
	}
	bool IsJcc() const {
		switch (insn->id) {
		case X86_INS_JAE: case X86_INS_JBE: case X86_INS_JA: case X86_INS_JB:
		case X86_INS_JE: case X86_INS_JNE: case X86_INS_JG: case X86_INS_JGE:
		case X86_INS_JL: case X86_INS_JLE: case X86_INS_JE: case X86_INS_JNE:
		case X86_INS_JS: case X86_INS_JNS: case X86_INS_JO: case X86_INS_JNO:
		case X86_INS_JP: case X86_INS_JNP:
			return true;
		}
		return false;
	}

	uint64_t address() const { return insn->address; }
	uint16_t size() const { return insn->size; }
	uint64_t NextAddress() const { return insn->address + insn->size; }
	unsigned int id() const { return insn->id; }
	const cs_x86_op& ops(int i) const { return insn->detail->x86.operands[i]; }
	uint8_t op_count() const { return insn->detail->x86.op_count; }
	const char* mnemonic() const { return insn->mnemonic; }
};

// ----------------------------------------------------------------------------
// FunctionAnalyzer
// ----------------------------------------------------------------------------
class FunctionAnalyzer {
public:
	struct ILResult {
		cs_insn* lastIns{ nullptr };
		std::unique_ptr<ILInstr> il;
	};

	FunctionAnalyzer(AnalyzedFnData* fnInfo, DartFunction* dartFn, AsmInstructions& asm_insns, DartApp& app)
		: fnInfo(fnInfo), dartFn(dartFn), asm_insns(asm_insns), app(app) {}

	void asm2il();
	void printInsnException(InsnException& e);

	struct ObjectPoolInstr {
		// dstReg is srcReg when isWrite is true
		A64::Register dstReg;
		VarItem item{};
		bool isWrite;
		bool IsSet() const { return dstReg.IsSet(); }
	};

	ObjectPoolInstr getObjectPoolInstruction(AsmIterator& insn);
	struct StoreLocalResult { int32_t fpOffset{ 0 }; A64::Register srcReg; };

	// prologue + parameters
	void handlePrologue(AsmIterator& insn, uint64_t endPrologueAddr);
	std::tuple<A64::Register, A64::Register> unboxParam(AsmIterator& insn, A64::Register expectedSrcReg = A64::Register{});
	void handleFixedParameters(AsmIterator& insn);
	void handleOptionalPositionalParameters(AsmIterator& insn);
	void handleOptionalNamedParameters(AsmIterator& insn);
	void handleArgumentsDescriptorTypeArguments(AsmIterator& insn);
	std::unique_ptr<SetupParametersInstr> processPrologueParametersInstr(AsmIterator& insn, uint64_t endPrologueAddr);
	StoreLocalResult handleStoreLocal(AsmIterator& insn, A64::Register expectedSrcReg = A64::Register{});

	// matchers
	std::unique_ptr<EnterFrameInstr> processEnterFrameInstr(AsmIterator& insn);
	std::unique_ptr<LeaveFrameInstr> processLeaveFrameInstr(AsmIterator& insn);
	std::unique_ptr<AllocateStackInstr> processAllocateStackInstr(AsmIterator& insn);
	std::unique_ptr<CheckStackOverflowInstr> processCheckStackOverflowInstr(AsmIterator& insn);
	std::unique_ptr<CallLeafRuntimeInstr> processCallLeafRuntime(AsmIterator& insn);
	std::unique_ptr<ILInstr> processObjectPoolInstr(AsmIterator& insn);
	std::unique_ptr<LoadValueInstr> processLoadValueNoObjectPoolInstr(AsmIterator& insn);
	std::unique_ptr<LoadValueInstr> processLoadValueInstr(AsmIterator& insn);
	std::unique_ptr<ClosureCallInstr> processClosureCallInstr(AsmIterator& insn);
	std::unique_ptr<MoveRegInstr> processMoveRegInstr(AsmIterator& insn);
	std::unique_ptr<DecompressPointerInstr> processDecompressPointerInstr(AsmIterator& insn);
	std::unique_ptr<SaveRegisterInstr> processSaveRegisterInstr(AsmIterator& insn);
	std::unique_ptr<RestoreRegisterInstr> processLoadSavedRegisterInstr(AsmIterator& insn);
	std::unique_ptr<InitAsyncInstr> processInitAsyncInstr(AsmIterator& insn);
	std::unique_ptr<CallInstr> processCallInstr(AsmIterator& insn);
	std::unique_ptr<GdtCallInstr> processGdtCallInstr(AsmIterator& insn);
	std::unique_ptr<ReturnInstr> processReturnInstr(AsmIterator& insn);
	std::unique_ptr<TestTypeInstr> processInstanceofNoTypeArgumentInstr(AsmIterator& insn);
	std::unique_ptr<BranchIfSmiInstr> processBranchIfSmiInstr(AsmIterator& insn);
	std::unique_ptr<LoadClassIdInstr> processLoadClassIdInstr(AsmIterator& insn);
	std::unique_ptr<BoxInt64Instr> processBoxInt64Instr(AsmIterator& insn);
	std::unique_ptr<LoadInt32Instr> processLoadInt32FromBoxOrSmiInstrFromSrcReg(AsmIterator& insn, A64::Register expectedSrcReg = A64::Register{});
	std::unique_ptr<LoadInt32Instr> processLoadInt32FromBoxOrSmiInstr(AsmIterator& insn);
	std::unique_ptr<LoadTaggedClassIdMayBeSmiInstr> processLoadTaggedClassIdMayBeSmiInstr(AsmIterator& insn);
	std::unique_ptr<ILInstr> processLoadFieldTableInstr(AsmIterator& insn);
	std::unique_ptr<AllocateObjectInstr> processTryAllocateObject(AsmIterator& insn);
	std::unique_ptr<WriteBarrierInstr> processWriteBarrierInstr(AsmIterator& insn);
	std::unique_ptr<ILInstr> processLoadStore(AsmIterator& insn);

private:
	void setAsmTextDataPool(uint64_t addr, uint64_t offset) {
		auto& asm_text = fnInfo->asmTexts.AtAddr(addr);
		asm_text.dataType = AsmText::PoolOffset;
		asm_text.poolOffset = offset;
	}
	void setAsmTextDataBoolean(uint64_t addr, bool b) {
		auto& asm_text = fnInfo->asmTexts.AtAddr(addr);
		asm_text.dataType = AsmText::Boolean;
		asm_text.boolVal = b;
	}
	void setAsmTextDataCall(uint64_t addr, uint64_t callAddress) {
		auto& asm_text = fnInfo->asmTexts.AtAddr(addr);
		asm_text.dataType = AsmText::Call;
		asm_text.callAddress = callAddress;
	}

	AnalyzedFnData* fnInfo;
	DartFunction* dartFn;
	AsmInstructions& asm_insns;
	DartApp& app;
};

typedef std::unique_ptr<ILInstr>(FunctionAnalyzer::* AsmMatcherFn)(AsmIterator& insn);
static const AsmMatcherFn matcherFns[] = {
	(AsmMatcherFn) &FunctionAnalyzer::processEnterFrameInstr,
	(AsmMatcherFn) &FunctionAnalyzer::processLeaveFrameInstr,
	(AsmMatcherFn) &FunctionAnalyzer::processAllocateStackInstr,
	(AsmMatcherFn) &FunctionAnalyzer::processCheckStackOverflowInstr,
	(AsmMatcherFn) &FunctionAnalyzer::processCallLeafRuntime,
	(AsmMatcherFn) &FunctionAnalyzer::processObjectPoolInstr,
	(AsmMatcherFn) &FunctionAnalyzer::processLoadValueNoObjectPoolInstr,
	(AsmMatcherFn) &FunctionAnalyzer::processDecompressPointerInstr,
	(AsmMatcherFn) &FunctionAnalyzer::processClosureCallInstr,
	(AsmMatcherFn) &FunctionAnalyzer::processSaveRegisterInstr,
	(AsmMatcherFn) &FunctionAnalyzer::processLoadSavedRegisterInstr,
	(AsmMatcherFn) &FunctionAnalyzer::processInitAsyncInstr,
	(AsmMatcherFn) &FunctionAnalyzer::processCallInstr,
	(AsmMatcherFn) &FunctionAnalyzer::processGdtCallInstr,
	(AsmMatcherFn) &FunctionAnalyzer::processReturnInstr,
	(AsmMatcherFn) &FunctionAnalyzer::processInstanceofNoTypeArgumentInstr,
	(AsmMatcherFn) &FunctionAnalyzer::processBranchIfSmiInstr,
	(AsmMatcherFn) &FunctionAnalyzer::processLoadClassIdInstr,
	(AsmMatcherFn) &FunctionAnalyzer::processBoxInt64Instr,
	(AsmMatcherFn) &FunctionAnalyzer::processLoadInt32FromBoxOrSmiInstr,
	(AsmMatcherFn) &FunctionAnalyzer::processLoadTaggedClassIdMayBeSmiInstr,
	(AsmMatcherFn) &FunctionAnalyzer::processLoadFieldTableInstr,
	(AsmMatcherFn) &FunctionAnalyzer::processTryAllocateObject,
	(AsmMatcherFn) &FunctionAnalyzer::processWriteBarrierInstr,
	&FunctionAnalyzer::processLoadStore,
};

FunctionAnalyzer::ObjectPoolInstr FunctionAnalyzer::getObjectPoolInstruction(AsmIterator& insn)
{
	// x64 AOT: object pool entries are 8-byte full pointers at [PP + disp32],
	// PP (R15) is tagged on x64, so Capstone disp == element_offset - kHeapObjectTag.
	// disp32 signed covers the whole pool, no multi-instruction expansion needed.
	if (insn.id() == X86_INS_MOV) {
		if (insn.op_count() == 2) {
			const auto& op0 = insn.ops(0);
			const auto& op1 = insn.ops(1);
			if (IsX86Mem(op1) && IsCsDartPp((x86_reg)op1.mem.base) && op1.mem.index == X86_REG_INVALID) {
				if (IsX86Reg(op0)) {
					// movq dst, [PP + disp]
					const auto dstReg = A64::Register{ op0.reg };
					++insn;
					return ObjectPoolInstr{ dstReg, VarItem{ VarStorage::NewPool((int)op1.mem.disp), getPoolObject(app, op1.mem.disp, dstReg) }, false };
				}
				if (IsX86Mem(op0) && IsX86Reg(op1)) {
					// movq [PP + disp], src  (store to pool)
					// object pool store is not expected in AOT code; guard anyway
					return ObjectPoolInstr{ A64::Register{ op1.reg }, VarItem{}, true };
				}
			}
		}
	}
	return ObjectPoolInstr{};
}

std::unique_ptr<EnterFrameInstr> FunctionAnalyzer::processEnterFrameInstr(AsmIterator& insn)
{
	// push rbp ; mov rbp, rsp
	if (insn.id() == X86_INS_PUSH && insn.op_count() == 1 && IsX86Reg(insn.ops(0)) && insn.ops(0).reg == X86_REG_RBP) {
		const auto ins0_addr = insn.address();
		++insn;

		if (!(insn.id() == X86_INS_MOV && insn.op_count() == 2 &&
			IsX86Reg(insn.ops(0)) && insn.ops(0).reg == X86_REG_RBP &&
			IsX86Reg(insn.ops(1)) && insn.ops(1).reg == X86_REG_RSP))
			return nullptr;

		++insn;
		fnInfo->useFramePointer = true;
		return std::make_unique<EnterFrameInstr>(insn.Wrap(ins0_addr));
	}
	return nullptr;
}

std::unique_ptr<LeaveFrameInstr> FunctionAnalyzer::processLeaveFrameInstr(AsmIterator& insn)
{
	// mov rsp, rbp ; pop rbp
	if (insn.id() == X86_INS_MOV && insn.op_count() == 2 &&
		IsX86Reg(insn.ops(0)) && insn.ops(0).reg == X86_REG_RSP &&
		IsX86Reg(insn.ops(1)) && insn.ops(1).reg == X86_REG_RBP) {
		INSN_ASSERT(fnInfo->useFramePointer);
		const auto ins0_addr = insn.address();
		++insn;

		if (!(insn.id() == X86_INS_POP && insn.op_count() == 1 && IsX86Reg(insn.ops(0)) && insn.ops(0).reg == X86_REG_RBP))
			return nullptr;
		++insn;

		return std::make_unique<LeaveFrameInstr>(insn.Wrap(ins0_addr));
	}
	return nullptr;
}

std::unique_ptr<AllocateStackInstr> FunctionAnalyzer::processAllocateStackInstr(AsmIterator& insn)
{
	// sub rsp, imm
	if (insn.id() == X86_INS_SUB && insn.op_count() == 2 &&
		IsX86Reg(insn.ops(0)) && insn.ops(0).reg == X86_REG_RSP &&
		IsX86Imm(insn.ops(1))) {
		const auto stackSize = (uint32_t)insn.ops(1).imm;
		fnInfo->stackSize = stackSize;
		const auto ins0_addr = insn.address();
		++insn;
		return std::make_unique<AllocateStackInstr>(insn.Wrap(ins0_addr), stackSize);
	}
	return nullptr;
}

std::unique_ptr<CheckStackOverflowInstr> FunctionAnalyzer::processCheckStackOverflowInstr(AsmIterator& insn)
{
	// cmp rsp, [r14 + stack_limit_offset]
	// jbe <slow_path>
	if (insn.id() == X86_INS_CMP && insn.op_count() == 2 &&
		IsX86Reg(insn.ops(0)) && insn.ops(0).reg == X86_REG_RSP &&
		IsX86MemDisp(insn.ops(1), X86_REG_R14, AOT_Thread_stack_limit_offset)) {
		const auto ins0_addr = insn.address();
		++insn;

		if (!(insn.id() == X86_INS_JBE))
			return nullptr;
		const auto target = insn.BranchTarget();
		++insn;

		if (target != 0) {
			// the dart compiler always put slow path at the end of function after "ret"
			INSN_ASSERT(target < dartFn->AddressEnd() && target >= insn.address());
			if (fnInfo->firstCheckStackOverflowAddr == 0)
				fnInfo->firstCheckStackOverflowAddr = ins0_addr;
			return std::make_unique<CheckStackOverflowInstr>(insn.Wrap(ins0_addr), target);
		}
	}
	return nullptr;
}

std::unique_ptr<MoveRegInstr> FunctionAnalyzer::processMoveRegInstr(AsmIterator& insn)
{
	if (insn.id() == X86_INS_MOV && insn.op_count() == 2 && IsX86Reg(insn.ops(0)) && IsX86Reg(insn.ops(1))) {
		const auto ins0_addr = insn.address();
		const A64::Register dstReg = insn.ops(0).reg;
		const A64::Register srcReg = insn.ops(1).reg;
		++insn;
		return std::make_unique<MoveRegInstr>(insn.Wrap(ins0_addr), dstReg, srcReg);
	}
	return nullptr;
}

std::unique_ptr<SaveRegisterInstr> FunctionAnalyzer::processSaveRegisterInstr(AsmIterator& insn)
{
	if (insn.id() == X86_INS_PUSH && insn.op_count() == 1 && IsX86Reg(insn.ops(0))) {
		const auto reg = A64::Register{ insn.ops(0).reg };
		const auto ins0_addr = insn.address();
		++insn;
		return std::make_unique<SaveRegisterInstr>(insn.Wrap(ins0_addr), reg);
	}
	return nullptr;
}

std::unique_ptr<RestoreRegisterInstr> FunctionAnalyzer::processLoadSavedRegisterInstr(AsmIterator& insn)
{
	if (insn.id() == X86_INS_POP && insn.op_count() == 1 && IsX86Reg(insn.ops(0))) {
		const auto reg = A64::Register{ insn.ops(0).reg };
		const auto ins0_addr = insn.address();
		++insn;
		return std::make_unique<RestoreRegisterInstr>(insn.Wrap(ins0_addr), reg);
	}
	return nullptr;
}

std::unique_ptr<DecompressPointerInstr> FunctionAnalyzer::processDecompressPointerInstr(AsmIterator& insn)
{
	// addq reg, [r14 + heap_base_offset]  (compressed pointer decompression)
	if (insn.id() == X86_INS_ADD && insn.op_count() == 2 &&
		IsX86Reg(insn.ops(0)) && IsX86Reg(insn.ops(1)) && insn.ops(0).reg == insn.ops(1).reg &&
		IsX86MemDisp(insn.ops(1), X86_REG_R14, AOT_Thread_heap_base_offset)) {
		const auto reg = A64::Register{ insn.ops(0).reg };
		const auto ins0_addr = insn.address();
		++insn;
		return std::make_unique<DecompressPointerInstr>(insn.Wrap(ins0_addr), VarStorage::NewRegister(reg));
	}
	return nullptr;
}

std::unique_ptr<LoadValueInstr> FunctionAnalyzer::processLoadValueNoObjectPoolInstr(AsmIterator& insn)
{
	const auto ins0_addr = insn.address();
	A64::Register dstReg;
	int64_t imm = 0;

	if (insn.id() == X86_INS_MOV || insn.id() == X86_INS_MOVABS) {
		if (insn.op_count() == 2 && IsX86Reg(insn.ops(0)) && IsX86Imm(insn.ops(1))) {
			imm = (int64_t)insn.ops(1).imm;
			dstReg = A64::Register{ insn.ops(0).reg };
			++insn;
		}
	}
	else if (insn.id() == X86_INS_XOR && insn.op_count() == 2 && IsX86Reg(insn.ops(0)) && IsX86Reg(insn.ops(1)) && insn.ops(0).reg == insn.ops(1).reg) {
		// xor reg, reg  => 0
		dstReg = A64::Register{ insn.ops(0).reg };
		++insn;
	}

	if (dstReg.IsSet()) {
		auto item = VarItem{ VarStorage::Immediate, new VarInteger{imm, VarValue::NativeInt} };
		return std::make_unique<LoadValueInstr>(insn.Wrap(ins0_addr), dstReg, std::move(item));
	}

	return nullptr;
}

std::unique_ptr<CallInstr> FunctionAnalyzer::processCallInstr(AsmIterator& insn)
{
	if (insn.id() == X86_INS_CALL && insn.op_count() == 1 && IsX86Imm(insn.ops(0))) {
		const auto target = (uint64_t)insn.ops(0).imm;
		const auto ins0_addr = insn.address();
		setAsmTextDataCall(ins0_addr, target);
		++insn;
		return std::make_unique<CallInstr>(insn.Wrap(ins0_addr), app.GetFunction(target), target);
	}
	else if (insn.id() == X86_INS_JMP && insn.op_count() == 1 && IsX86Imm(insn.ops(0))) {
		// tail branch (function call at the end of function)
		const auto target = (uint64_t)insn.ops(0).imm;
		if (target < dartFn->Address() || target >= dartFn->AddressEnd()) {
			const auto ins0_addr = insn.address();
			setAsmTextDataCall(ins0_addr, target);
			++insn;
			return std::make_unique<CallInstr>(insn.Wrap(ins0_addr), app.GetFunction(target), target);
		}
	}

	return nullptr;
}

std::unique_ptr<GdtCallInstr> FunctionAnalyzer::processGdtCallInstr(AsmIterator& insn)
{
	// FlowGraphCompiler::EmitDispatchTableCall() (x64):
	//   movq rax, [THR + dispatch_table_array_offset]   ; LoadDispatchTable
	//   call [rax + rcx*8 + disp]                       ; disp = (selector_offset - origin) * 8
	InsnMarker marker(insn);
	if (!(insn.id() == X86_INS_MOV && insn.op_count() == 2 &&
		IsX86Reg(insn.ops(0)) && insn.ops(0).reg == X86_REG_RAX &&
		IsX86MemDisp(insn.ops(1), X86_REG_R14, AOT_Thread_dispatch_table_array_offset)))
		return nullptr;
	++insn;

	if (!(insn.id() == X86_INS_CALL && insn.op_count() == 1 && IsX86Mem(insn.ops(0))))
		return nullptr;
	const auto& mem = insn.ops(0).mem;
	if (!(mem.base == X86_REG_RAX && mem.index == X86_REG_RCX && mem.scale == 8))
		return nullptr;
	if (mem.disp % 8 != 0)
		return nullptr;
	const int64_t offset = mem.disp / 8;
	++insn;

	return std::make_unique<GdtCallInstr>(insn.Wrap(marker.Take()), offset);
}

std::unique_ptr<ReturnInstr> FunctionAnalyzer::processReturnInstr(AsmIterator& insn)
{
	if (insn.id() == X86_INS_RET) {
		const auto ins0_addr = insn.address();
		++insn;
		return std::make_unique<ReturnInstr>(insn.Wrap(ins0_addr));
	}
	return nullptr;
}

std::unique_ptr<BranchIfSmiInstr> FunctionAnalyzer::processBranchIfSmiInstr(AsmIterator& insn)
{
	// test reg, 1 ; je <branch>   (Smi has LSB cleared => ZF set on test)
	if (insn.id() == X86_INS_TEST && insn.op_count() == 2 &&
		IsX86Reg(insn.ops(0)) && IsX86Imm(insn.ops(1)) && insn.ops(1).imm == dart::kSmiTagMask) {
		const auto objReg = A64::Register{ insn.ops(0).reg };
		const auto ins0_addr = insn.address();
		++insn;

		if (!(insn.id() == X86_INS_JE || insn.id() == X86_INS_JE))
			return nullptr;
		const auto branchAddr = insn.BranchTarget();
		++insn;

		return std::make_unique<BranchIfSmiInstr>(insn.Wrap(ins0_addr), objReg, branchAddr);
	}
	return nullptr;
}

std::unique_ptr<LoadClassIdInstr> FunctionAnalyzer::processLoadClassIdInstr(AsmIterator& insn)
{
	// movl cid, [obj + tags_offset - kHeapObjectTag]  (32-bit)
	// shrl cid, kClassIdTagPos
	if (kUntaggedObjectClassIdTagPos == 12 && insn.id() == X86_INS_MOV && insn.op_count() == 2 &&
		IsX86Reg(insn.ops(0)) && IsX86Mem(insn.ops(1)) && insn.ops(0).size == 4) {
		const auto objReg = A64::Register{ insn.ops(1).mem.base };
		const auto cidReg = A64::Register{ insn.ops(0).reg };
		const auto ins0_addr = insn.address();
		++insn;

		// Assembler::LoadClassId(): shrl result, kClassIdTagPos
		if (!(insn.id() == X86_INS_SHR && insn.op_count() == 2 &&
			IsX86Reg(insn.ops(0)) && insn.ops(0).reg == insn.ops(1).reg && insn.ops(0).size == 4 &&
			IsX86Imm(insn.ops(1)) && insn.ops(1).imm == kUntaggedObjectClassIdTagPos))
			return nullptr;
		++insn;

		return std::make_unique<LoadClassIdInstr>(insn.Wrap(ins0_addr), objReg, A64::Register{cidReg});
	}
	return nullptr;
}

std::unique_ptr<BoxInt64Instr> FunctionAnalyzer::processBoxInt64Instr(AsmIterator& insn)
{
	// BoxInt64Instr::EmitNativeCode() (x64)
	// non-compressed pointers (Flutter Windows):
	//   movq out, value          ; MoveRegister (omitted when out == value)
	//   addq out, out            ; SmiTag
	//   [ValueFitsSmi() -> stop here]
	//   jno <done>               ; OF=0 => fits in Smi
	//   call <AllocateMintShared*Stub>   (slow path)
	//   movq [out + Mint::value_offset - tag], value
	// <done>:
	InsnMarker marker(insn);
	A64::Register out_reg;
	A64::Register in_reg;

	if (insn.id() == X86_INS_ADD && insn.op_count() == 2 && IsX86Reg(insn.ops(0)) && IsX86Reg(insn.ops(1)) &&
		insn.ops(0).size == 8 && insn.ops(0).reg == insn.ops(1).reg) {
		// out == value (MoveRegister omitted)
		out_reg = A64::Register{ insn.ops(0).reg };
		in_reg = out_reg;
		++insn;
	}
	else if (insn.id() == X86_INS_MOV && insn.op_count() == 2 && IsX86Reg(insn.ops(0)) && IsX86Reg(insn.ops(1))) {
		// movq out, value
		out_reg = A64::Register{ insn.ops(0).reg };
		in_reg = A64::Register{ insn.ops(1).reg };
		++insn;
		if (!(insn.id() == X86_INS_ADD && insn.op_count() == 2 && IsX86Reg(insn.ops(0)) && IsX86Reg(insn.ops(1)) &&
			insn.ops(0).size == 8 && insn.ops(0).reg == out_reg && insn.ops(1).reg == out_reg))
			return nullptr;
		++insn;
	}
	else {
		return nullptr;
	}

	// fast path: value fits in Smi (only the tag was emitted)
	if (!(insn.id() == X86_INS_JNO))
		return std::make_unique<BoxInt64Instr>(insn.Wrap(marker.Take()), out_reg, in_reg);
	++insn;

	// slow path: call AllocateMint stub, then store value into Mint
	const auto assertAllocateMintStub = [&](DartFnBase* stub) {
		INSN_ASSERT(stub->IsStub());
		const auto stubKind = reinterpret_cast<DartStub*>(stub)->kind;
		INSN_ASSERT(stubKind == DartStub::AllocateMintSharedWithoutFPURegsStub || stubKind == DartStub::AllocateMintSharedWithFPURegsStub);
	};

	if (insn.id() == X86_INS_CALL && insn.op_count() == 1 && IsX86Imm(insn.ops(0))) {
		assertAllocateMintStub(app.GetFunction(insn.ops(0).imm));
		setAsmTextDataCall(insn.address(), (uint64_t)insn.ops(0).imm);
		++insn;
	}
	else {
		// pool loaded stub call: load CODE then call [CODE + entry_point]
		const auto objPoolInstr = getObjectPoolInstruction(insn);
		INSN_ASSERT(objPoolInstr.IsSet());
		INSN_ASSERT(objPoolInstr.dstReg == A64::Register{ dart::CODE_REG });
		INSN_ASSERT(objPoolInstr.item.ValueTypeId() == dart::kFunctionCid);
		assertAllocateMintStub(&objPoolInstr.item.Get<VarFunctionCode>()->fn);

		INSN_ASSERT(insn.id() == X86_INS_CALL && insn.op_count() == 1 && IsX86Mem(insn.ops(0)));
		++insn;
	}

	if (!(insn.id() == X86_INS_MOV && insn.op_count() == 2 && IsX86Mem(insn.ops(0)) && IsX86Reg(insn.ops(1)) &&
		insn.ops(1).reg == in_reg))
		return nullptr;
	const auto& store_mem = insn.ops(0).mem;
	if (!((x86_reg)store_mem.base == out_reg && store_mem.disp == dart::Mint::value_offset() - dart::kHeapObjectTag))
		return nullptr;
	++insn;

	return std::make_unique<BoxInt64Instr>(insn.Wrap(marker.Take()), out_reg, in_reg);
}

std::unique_ptr<LoadInt32Instr> FunctionAnalyzer::processLoadInt32FromBoxOrSmiInstrFromSrcReg(AsmIterator& insn, A64::Register expectedSrcReg)
{
	// Assembler::LoadInt32FromBoxOrSmi() (x64, non-compressed pointers):
	//   sarq out, 1            ; SmiUntag, CF = original LSB
	//   jae <done>            ; NOT_CARRY (CF=0) <=> Smi
	//   movsxd out, [out*2 + Mint::value_offset - kHeapObjectTag]   ; Mint case
	// <done>:
	if (insn.id() == X86_INS_SAR && insn.op_count() == 2 &&
		IsX86Reg(insn.ops(0)) && insn.ops(0).size == 8 &&
		IsX86Imm(insn.ops(1)) && insn.ops(1).imm == dart::kSmiTagSize) {
		const auto out_reg = A64::Register{ insn.ops(0).reg };
		const auto srcReg = out_reg; // in-place operation (value == result)
		if (expectedSrcReg.IsSet() && expectedSrcReg != srcReg)
			return nullptr;
		const auto ins0_addr = insn.address();
		++insn;

		if (insn.id() == X86_INS_JAE) {
			// non-Smi case: load raw integer from Mint object
			const auto cont_addr = insn.BranchTarget();
			++insn;

			INSN_ASSERT(insn.id() == X86_INS_MOVSXD && insn.op_count() == 2 && IsX86Reg(insn.ops(0)) && IsX86Mem(insn.ops(1)));
			INSN_ASSERT(insn.ops(0).reg == out_reg);
			const auto& mem = insn.ops(1).mem;
			// [out + out + mint_value_offset - tag]  (untagged value * 2)
			INSN_ASSERT((x86_reg)mem.base == out_reg && (x86_reg)mem.index == out_reg && mem.scale == 1 &&
				mem.disp == dart::Mint::value_offset() - dart::kHeapObjectTag);
			++insn;

			INSN_ASSERT(insn.address() == cont_addr);
		}
		else {
			// srcReg object is always Smi
		}

		return std::make_unique<LoadInt32Instr>(insn.Wrap(ins0_addr), out_reg, srcReg);
	}
	return nullptr;
}

std::unique_ptr<LoadInt32Instr> FunctionAnalyzer::processLoadInt32FromBoxOrSmiInstr(AsmIterator& insn)
{
	return processLoadInt32FromBoxOrSmiInstrFromSrcReg(insn);
}

std::unique_ptr<LoadTaggedClassIdMayBeSmiInstr> FunctionAnalyzer::processLoadTaggedClassIdMayBeSmiInstr(AsmIterator& insn)
{
	//   cidReg = SmiTaggedClassId
	//   branchIfSmi(objReg, <smi>)
	//   cidReg = LoadClassIdInstr(objReg)
	//   shl cidReg, cidReg, 1     ; SmiTag
	// <smi>:
	if (insn.id() == X86_INS_SHL && insn.op_count() == 2 &&
		IsX86Reg(insn.ops(0)) && insn.ops(0).size == 8 &&
		IsX86Imm(insn.ops(1)) && insn.ops(1).imm == dart::kSmiTagSize) {
		auto& il = fnInfo->il_insns.back();
		if (il->Kind() != ILInstr::LoadClassId || fnInfo->il_insns.size() < 3)
			return nullptr;
		auto il_loadClassId = reinterpret_cast<LoadClassIdInstr*>(il.get());
		if (il_loadClassId->cidReg != A64::Register{ insn.ops(0).reg })
			return nullptr;

		auto& il2 = fnInfo->il_insns[fnInfo->il_insns.size() - 2];
		if (il2->Kind() != ILInstr::BranchIfSmi)
			return nullptr;
		auto il_branchIfSmi = reinterpret_cast<BranchIfSmiInstr*>(il2.get());
		if (il_branchIfSmi->objReg != il_loadClassId->objReg)
			return nullptr;

		auto& il3 = fnInfo->il_insns[fnInfo->il_insns.size() - 3];
		if (il3->Kind() != ILInstr::LoadValue)
			return nullptr;
		auto il_loadImm = reinterpret_cast<LoadValueInstr*>(il3.get());
		if (il_loadImm->dstReg != il_loadClassId->cidReg)
			return nullptr;
		if (!il_loadImm->val.Storage().IsImmediate())
			return nullptr;
		if (il_loadImm->val.ValueTypeId() != dart::kIntegerCid)
			return nullptr;
		if (il_loadImm->val.Get<VarInteger>()->Value() != dart::Smi::RawValue(dart::kSmiCid))
			return nullptr;

		// everything is OK, release all IL to cast to specific IL
		il.release();
		il2.release();
		il3.release();
		++insn;
		auto il_new = std::make_unique<LoadTaggedClassIdMayBeSmiInstr>(insn.Wrap(il_loadImm->Start()),
			std::unique_ptr<LoadValueInstr>(il_loadImm), std::unique_ptr<BranchIfSmiInstr>(il_branchIfSmi),
			std::unique_ptr<LoadClassIdInstr>(il_loadClassId));
		fnInfo->il_insns.resize(fnInfo->il_insns.size() - 3);
		return std::move(il_new);
	}
	return nullptr;
}

std::unique_ptr<ILInstr> FunctionAnalyzer::processLoadFieldTableInstr(AsmIterator& insn)
{
	// LoadStaticFieldInstr::EmitNativeCode() / StoreStaticFieldInstr::EmitNativeCode() (x64)
	//   movq reg, [THR + field_table_values_offset]
	//   movq reg, [reg + Smi(field_offset)]   ; load  (Smi field offset)
	//   movq [reg + Smi(field_offset)], reg   ; store
	if (insn.id() == X86_INS_MOV && insn.op_count() == 2 &&
		IsX86Reg(insn.ops(0)) && IsX86MemDisp(insn.ops(1), X86_REG_R14, AOT_Thread_field_table_values_offset)) {
		const auto result_reg = insn.ops(0).reg;
		const auto dstReg = A64::Register{ result_reg };
		InsnMarker marker(insn);
		++insn;

		if (insn.id() == X86_INS_MOV && insn.op_count() == 2 && IsX86Mem(insn.ops(0)) && IsX86Reg(insn.ops(1))) {
			// store: movq [table_reg + off], value
			const auto& mem = insn.ops(0).mem;
			if (!((x86_reg)mem.base == result_reg && mem.disp % 2 == 0))
				return nullptr;
			const auto field_offset = mem.disp / 2;
			const auto reg = A64::Register{ insn.ops(1).reg };
			++insn;
			return std::make_unique<StoreStaticFieldInstr>(insn.Wrap(marker.Take()), reg, field_offset);
		}

		if (insn.id() == X86_INS_MOV && insn.op_count() == 2 && IsX86Reg(insn.ops(0)) && IsX86Mem(insn.ops(1))) {
			// load: movq reg, [table_reg + off]
			const auto& mem = insn.ops(1).mem;
			if (!((x86_reg)mem.base == result_reg && mem.disp % 2 == 0))
				return nullptr;
			const auto field_offset = mem.disp / 2;
			if (insn.ops(0).reg != result_reg)
				return nullptr;
			++insn;
			return std::make_unique<LoadStaticFieldInstr>(insn.Wrap(marker.Take()), dstReg, field_offset);
		}
	}
	return nullptr;
}

std::unique_ptr<AllocateObjectInstr> FunctionAnalyzer::processTryAllocateObject(AsmIterator& insn)
{
	// Assembler::TryAllocateObject() with inline_alloc (x64)
	//   movq inst, [THR + top]
	//   addq inst, instance_size
	//   cmpq inst, [THR + end]
	//   jae <slow_path>
	//   movq [THR + top], inst
	//   addq inst, kHeapObjectTag - instance_size
	//   movq [inst + tags_offset - tag], tags_imm
	if (insn.id() == X86_INS_MOV && insn.op_count() == 2 &&
		IsX86Reg(insn.ops(0)) && IsX86MemDisp(insn.ops(1), X86_REG_R14, AOT_Thread_top_offset)) {
		const auto inst_reg = insn.ops(0).reg;
		InsnMarker marker(insn);
		++insn;

		if (!(insn.id() == X86_INS_ADD && insn.op_count() == 2 && IsX86Reg(insn.ops(0)) && IsX86Reg(insn.ops(1)) &&
			insn.ops(0).reg == inst_reg && insn.ops(1).reg == inst_reg && IsX86Imm(insn.ops(1)) == false))
			return nullptr;
		const auto inst_size = insn.ops(1).imm;
		++insn;

		if (!(insn.id() == X86_INS_CMP && insn.op_count() == 2 && IsX86Reg(insn.ops(0)) && insn.ops(0).reg == inst_reg &&
			IsX86MemDisp(insn.ops(1), X86_REG_R14, AOT_Thread_end_offset)))
			return nullptr;
		++insn;

		if (!(insn.id() == X86_INS_JAE))
			return nullptr;
		const uint64_t slow_path = insn.BranchTarget();
		INSN_ASSERT(insn.address() < slow_path && slow_path < dartFn->AddressEnd());
		++insn;

		if (!(insn.id() == X86_INS_MOV && insn.op_count() == 2 && IsX86Mem(insn.ops(0)) && IsX86Reg(insn.ops(1)) &&
			IsX86MemDisp(insn.ops(0), X86_REG_R14, AOT_Thread_top_offset) && insn.ops(1).reg == inst_reg))
			return nullptr;
		++insn;

		if (!(insn.id() == X86_INS_ADD && insn.op_count() == 2 && IsX86Reg(insn.ops(0)) && IsX86Reg(insn.ops(1)) &&
			insn.ops(0).reg == inst_reg && insn.ops(1).reg == inst_reg &&
			IsX86Imm(insn.ops(1)) && insn.ops(1).imm == (dart::kHeapObjectTag - inst_size)))
			return nullptr;
		++insn;

		if (!(insn.id() == X86_INS_MOV && insn.op_count() == 2 && IsX86Mem(insn.ops(0)) && IsX86Imm(insn.ops(1)) &&
			(x86_reg)insn.ops(0).mem.base == inst_reg))
			return nullptr;
		const auto tag = insn.ops(1).imm;
		++insn;

		const uint32_t cid = (tag >> kUntaggedObjectClassIdTagPos) & ((1 << dart::UntaggedObject::kClassIdTagSize) - 1);
		auto dartCls = app.GetClass(cid);

		const auto dstReg = A64::Register{ inst_reg };
		return std::make_unique<AllocateObjectInstr>(insn.Wrap(marker.Take()), dstReg, *dartCls);
	}
	return nullptr;
}

std::unique_ptr<ILInstr> FunctionAnalyzer::processObjectPoolInstr(AsmIterator& insn)
{
	const auto ins0_addr = insn.address();
	auto objPoolInstr = getObjectPoolInstruction(insn);
	if (objPoolInstr.IsSet()) {
		if (objPoolInstr.isWrite) {
			return std::make_unique<StoreObjectPoolInstr>(insn.Wrap(ins0_addr), objPoolInstr.dstReg, objPoolInstr.item.storage.offset);
		}
		return std::make_unique<LoadValueInstr>(insn.Wrap(ins0_addr), objPoolInstr.dstReg, std::move(objPoolInstr.item));
	}
	return nullptr;
}

std::unique_ptr<LoadValueInstr> FunctionAnalyzer::processLoadValueInstr(AsmIterator& insn)
{
	auto il = processObjectPoolInstr(insn);
	if (il != nullptr) {
		if (il->Kind() != ILInstr::LoadValue)
			return nullptr;
		return std::unique_ptr<LoadValueInstr>((LoadValueInstr*)il.release());
	}
	return processLoadValueNoObjectPoolInstr(insn);
}

std::unique_ptr<WriteBarrierInstr> FunctionAnalyzer::processWriteBarrierInstr(AsmIterator& insn)
{
	// Assembler::StoreBarrier() / StoreIntoArrayBarrier() (x64)
	//   [kValueCanBeSmi] test reg_val, 1 ; je <done>
	//   movb TMP, [obj + tags_offset - kHeapObjectTag]
	//   shrl TMP, kBarrierOverlapShift
	//   andl TMP, [THR + write_barrier_mask_offset]
	//   testb [val + tags_offset - kHeapObjectTag], TMP
	//   jz <done>
	//   call <write_barrier_wrappers_stub | array_write_barrier_stub>   (AOT: pc-relative)
	// <done>:
	InsnMarker marker(insn);

	uint64_t contSmiAddr = 0;
	A64::Register valReg;
	if (insn.id() == X86_INS_TEST && insn.op_count() == 2 && IsX86Reg(insn.ops(0)) && IsX86Imm(insn.ops(1)) && insn.ops(1).imm == dart::kSmiTagMask) {
		valReg = A64::Register{ insn.ops(0).reg };
		++insn;
		if (!(insn.id() == X86_INS_JE || insn.id() == X86_INS_JE))
			return nullptr;
		contSmiAddr = insn.BranchTarget();
		++insn;
	}

	// movb TMP, [obj + tags_offset - tag]  (tags field lives at -1)
	if (!(insn.id() == X86_INS_MOV && insn.op_count() == 2 && IsX86Reg(insn.ops(0)) && IsX86Mem(insn.ops(1)) &&
		insn.ops(0).size == 1 && insn.ops(0).reg == X86_REG_R11))
		return nullptr;
	if (insn.ops(1).mem.disp != -1)
		return nullptr;
	const auto objReg = A64::Register{ insn.ops(1).mem.base };
	++insn;

	if (!(insn.id() == X86_INS_SHR && insn.op_count() == 2 && IsX86Reg(insn.ops(0)) && insn.ops(0).reg == X86_REG_R11 && insn.ops(0).size == 4 && IsX86Imm(insn.ops(1))))
		return nullptr;
	const auto barrierOverlapShift = insn.ops(1).imm;
	++insn;

	if (!(insn.id() == X86_INS_AND && insn.op_count() == 2 && IsX86Reg(insn.ops(0)) && insn.ops(0).reg == X86_REG_R11 &&
		IsX86MemDisp(insn.ops(1), X86_REG_R14, AOT_Thread_write_barrier_mask_offset)))
		return nullptr;
	++insn;

	// testb [val + tags_offset - tag], TMP
	if (!(insn.id() == X86_INS_TEST && insn.op_count() == 2 && IsX86Mem(insn.ops(0)) && IsX86Reg(insn.ops(1)) &&
		insn.ops(1).reg == X86_REG_R11 && insn.ops(0).mem.disp == -1))
		return nullptr;
	if (!valReg.IsSet()) {
		valReg = A64::Register{ insn.ops(0).mem.base };
	}
	++insn;

	if (!(insn.id() == X86_INS_JE || insn.id() == X86_INS_JE))
		return nullptr;
	const auto contAddr = insn.BranchTarget();
	INSN_ASSERT(contSmiAddr == 0 || contSmiAddr == contAddr);
	++insn;

	bool isArray = false;
	if (insn.id() == X86_INS_CALL && insn.op_count() == 1 && IsX86Imm(insn.ops(0))) {
		// AOT: pc-relative call to the write barrier wrapper / array stub
		auto stub = app.GetFunction(insn.ops(0).imm);
		INSN_ASSERT(stub->IsStub());
		const auto stubKind = reinterpret_cast<DartStub*>(stub)->kind;
		INSN_ASSERT(stubKind == DartStub::WriteBarrierWrappersStub || stubKind == DartStub::ArrayWriteBarrierStub);
		isArray = stubKind == DartStub::ArrayWriteBarrierStub;
		setAsmTextDataCall(insn.address(), (uint64_t)insn.ops(0).imm);
		++insn;
	}
	else {
		// indirect call via THR entry point (non pc-relative fallback)
		INSN_ASSERT(insn.id() == X86_INS_CALL && insn.op_count() == 1 && IsX86Mem(insn.ops(0)));
		const auto& mem = insn.ops(0).mem;
		INSN_ASSERT((x86_reg)mem.base == X86_REG_R14);
		if (mem.disp == AOT_Thread_array_write_barrier_entry_point_offset) {
			isArray = true;
		}
		else {
			const auto existed = std::find(std::begin(AOT_Thread_write_barrier_wrappers_thread_offset),
				std::end(AOT_Thread_write_barrier_wrappers_thread_offset), mem.disp) != std::end(AOT_Thread_write_barrier_wrappers_thread_offset);
			INSN_ASSERT(existed);
			isArray = false;
		}
		++insn;
	}

	INSN_ASSERT(insn.address() == contAddr);

	return std::make_unique<WriteBarrierInstr>(insn.Wrap(marker.Take()), objReg, valReg, isArray);
}

std::unique_ptr<ClosureCallInstr> FunctionAnalyzer::processClosureCallInstr(AsmIterator& insn)
{
	// ClosureCallInstr::EmitNativeCode() (x64, AOT)
	//   movq rcx, [rax + Closure::entry_point_offset - kHeapObjectTag]   ; RAX: closure
	//   call rcx
	if (insn.id() == X86_INS_MOV && insn.op_count() == 2 && IsX86Reg(insn.ops(0)) && insn.ops(0).reg == X86_REG_RCX &&
		IsX86Mem(insn.ops(1)) && (x86_reg)insn.ops(1).mem.base == X86_REG_RAX &&
		insn.ops(1).mem.disp == AOT_Closure_entry_point_offset - dart::kHeapObjectTag) {
		// previous IL must be LoadValue of ArgumentsDescriptor with dstReg ARGS_DESC_REG
		auto il = fnInfo->LastIL();
		if (il->Kind() == ILInstr::LoadValue) {
			auto loadIL = reinterpret_cast<LoadValueInstr*>(il);
			if (loadIL->dstReg == A64::Register{ dart::ARGS_DESC_REG } && loadIL->val.ValueTypeId() == dart::kArrayCid) {
				const auto& arr = dart::Array::Handle(loadIL->val.Get<VarArray>()->ptr);
				const auto arrLen = arr.Length();
				const auto namedParamCmt = (arrLen - 5) / 2;
				auto arrPtr = dart::Array::DataOf(arr.ptr());

				INSN_ASSERT(!arrPtr->IsHeapObject());
				const auto typeArgLen = dart::RawSmiValue(dart::Smi::RawCast(arrPtr->DecompressSmi()));
				arrPtr++;
				INSN_ASSERT(!arrPtr->IsHeapObject());
				const auto argCnt = dart::RawSmiValue(dart::Smi::RawCast(arrPtr->DecompressSmi()));
				INSN_ASSERT(argCnt > 0);
				arrPtr++;
				INSN_ASSERT(!arrPtr->IsHeapObject());
				const auto argSize = dart::RawSmiValue(dart::Smi::RawCast(arrPtr->DecompressSmi()));
				INSN_ASSERT(argCnt == argSize);
				arrPtr++;
				INSN_ASSERT(!arrPtr->IsHeapObject());
				const auto positionalArgCnt = dart::RawSmiValue(dart::Smi::RawCast(arrPtr->DecompressSmi()));
				INSN_ASSERT(argCnt == positionalArgCnt + namedParamCmt);

				++insn;

				INSN_ASSERT(insn.id() == X86_INS_CALL && insn.op_count() == 1 && IsX86Reg(insn.ops(0)) && insn.ops(0).reg == X86_REG_RCX);
				++insn;

				const auto ins0_addr = il->Start();
				fnInfo->RemoveLastIL();

				return std::make_unique<ClosureCallInstr>(insn.Wrap(ins0_addr), (int32_t)argCnt, (int32_t)typeArgLen);
			}
		}
	}
	return nullptr;
}

std::unique_ptr<TestTypeInstr> FunctionAnalyzer::processInstanceofNoTypeArgumentInstr(AsmIterator& insn)
{
	// FlowGraphCompiler::GenerateInstanceOf() (x64, AOT)
	//   mov rax, src                        ; kInstanceReg
	//   xor rdx, rdx                        ; kInstantiatorTypeArgumentsReg = null
	//   xor rcx, rcx                        ; kFunctionTypeArgumentsReg = null
	//   test rax, 1; je <done>              ; BranchIfSmi
	//   movl tmp, [rax - 1]; shrl tmp, 12   ; LoadClassId (kScratchReg = RSI)
	//   sub tmp, kSmiCid
	//   cmp tmp, 1 | 2
	//   jbe <done>
	//   movq rbx, [PP + off]                ; kDstTypeReg (VarType)
	//   [movq r9, [PP + off] ; kSubtypeTestCacheReg (null)]
	//   call rel32                          ; TypeTestStub (or call [rbx + entry_point])
	// <done>:
	InsnMarker marker(insn);
	const auto srcReg = [&] {
		if (insn.id() == X86_INS_MOV && insn.op_count() == 2 && IsX86Reg(insn.ops(0)) && IsX86Reg(insn.ops(1)) &&
			insn.ops(0).reg == X86_REG_RAX) {
			const auto srcReg = A64::Register{ insn.ops(1).reg };
			++insn;
			// set kInstantiatorTypeArgumentsReg / kFunctionTypeArgumentsReg to null
			if (insn.id() == X86_INS_XOR && insn.op_count() == 2 && IsX86Reg(insn.ops(0)) && IsX86Reg(insn.ops(1)) &&
				insn.ops(0).reg == X86_REG_RDX && insn.ops(1).reg == X86_REG_RDX) {
				++insn;
				if (insn.id() == X86_INS_XOR && insn.op_count() == 2 && IsX86Reg(insn.ops(0)) && IsX86Reg(insn.ops(1)) &&
					insn.ops(0).reg == X86_REG_RCX && insn.ops(1).reg == X86_REG_RCX) {
					++insn;
					return srcReg;
				}
			}
		}
		return A64::Register{};
	}();

	if (srcReg.IsSet()) {
		// BranchIfSmi
		const auto ilBranch = processBranchIfSmiInstr(insn);
		if (!ilBranch)
			return nullptr;
		INSN_ASSERT(ilBranch->objReg == A64::Register{ dart::TypeTestABI::kInstanceReg });
		const auto done_addr = ilBranch->branchAddr;

		// int / num quick check
		intptr_t typeCheckCid = 0;
		const auto ilLoadCid = processLoadClassIdInstr(insn);
		if (ilLoadCid) {
			INSN_ASSERT(ilLoadCid->objReg == A64::Register{ dart::TypeTestABI::kInstanceReg });
			INSN_ASSERT(ilLoadCid->cidReg == A64::Register{ dart::TypeTestABI::kScratchReg });

			INSN_ASSERT(insn.id() == X86_INS_SUB && insn.op_count() == 2 && IsX86Reg(insn.ops(0)) && IsX86Imm(insn.ops(1)) &&
				insn.ops(0).reg == X86_REG_RSI && insn.ops(1).imm == dart::kSmiCid);
			++insn;

			INSN_ASSERT(insn.id() == X86_INS_CMP && insn.op_count() == 2 && IsX86Reg(insn.ops(0)) && IsX86Imm(insn.ops(1)) &&
				insn.ops(0).reg == X86_REG_RSI && (insn.ops(1).imm == 1 || insn.ops(1).imm == 2));
			typeCheckCid = insn.ops(1).imm == 1 ? app.DartIntCid() : dart::kNumberCid;
			++insn;

			INSN_ASSERT(insn.id() == X86_INS_JBE);
			INSN_ASSERT(insn.BranchTarget() == done_addr);
			++insn;
		}

		// kDstTypeReg from PP
		const auto objPoolInstr = getObjectPoolInstruction(insn);
		INSN_ASSERT(objPoolInstr.dstReg == A64::Register{ dart::TypeTestABI::kDstTypeReg });
		INSN_ASSERT(objPoolInstr.item.ValueTypeId() == dart::kTypeCid);
		const auto vtype = objPoolInstr.item.Get<VarType>();
		INSN_ASSERT(typeCheckCid == 0 || typeCheckCid == vtype->type.Class().Id());

		// optional: load type-test-stub entry point from RBX into a register,
		// or kSubtypeTestCacheReg (R9) from PP
		auto test_ep_reg = X86_REG_INVALID;
		if (insn.id() == X86_INS_MOV && insn.op_count() == 2 && IsX86Reg(insn.ops(0)) && IsX86Mem(insn.ops(1)) &&
			(x86_reg)insn.ops(1).mem.base == X86_REG_RBX) {
			test_ep_reg = insn.ops(0).reg;
			++insn;
		}

		if (test_ep_reg == X86_REG_INVALID) {
			// kSubtypeTestCacheReg may be loaded from PP (null cache) before the call
			if (insn.id() == X86_INS_MOV && insn.op_count() == 2 && IsX86Reg(insn.ops(0)) && IsX86Mem(insn.ops(1))) {
				const auto& mem = insn.ops(1).mem;
				if (IsCsDartPp((x86_reg)mem.base)) {
					++insn;
				}
			}
			INSN_ASSERT(insn.id() == X86_INS_CALL && insn.op_count() == 1 && IsX86Imm(insn.ops(0)));
			auto dartFn = app.GetFunction(insn.ops(0).imm);
			auto dartStub = dartFn->AsStub();
			auto typeName = dartStub->Name();
			if (typeCheckCid == app.DartIntCid()) {
				INSN_ASSERT(dartStub->kind == DartStub::TypeCheckStub);
				INSN_ASSERT(typeName == "int" || typeName == "int?");
			}
			else {
				INSN_ASSERT(typeName == vtype->ToString() || dartStub->kind == DartStub::DefaultTypeTestStub || dartStub->kind == DartStub::DefaultNullableTypeTestStub);
			}
			setAsmTextDataCall(insn.address(), (uint64_t)insn.ops(0).imm);
			++insn;
		}
		else {
			INSN_ASSERT(insn.id() == X86_INS_CALL && insn.op_count() == 1 && IsX86Reg(insn.ops(0)) && insn.ops(0).reg == test_ep_reg);
			++insn;
		}

		INSN_ASSERT(insn.address() == done_addr);

		return std::make_unique<TestTypeInstr>(insn.Wrap(marker.Take()), srcReg, objPoolInstr.item.Get<VarType>()->ToString());
	}

	return nullptr;
}

static ArrayOp getArrayOp(AsmIterator& insn, bool isLoad)
{
	// x64: element size and signedness from the memory-access instruction
	switch (insn.id()) {
	case X86_INS_MOV: {
		uint8_t size = 0;
		if (isLoad && insn.op_count() == 2 && IsX86Reg(insn.ops(0)) && IsX86Mem(insn.ops(1)))
			size = (uint8_t)insn.ops(0).size;
		else if (!isLoad && insn.op_count() == 2 && IsX86Mem(insn.ops(0)) && IsX86Reg(insn.ops(1)))
			size = (uint8_t)insn.ops(1).size;
		if (size == 4 || size == 8)
			return ArrayOp(size, isLoad, ArrayOp::Unknown);
		if (size == 1 || size == 2)
			return ArrayOp(size, isLoad, ArrayOp::TypedUnsigned);
		return ArrayOp();
	}
	case X86_INS_MOVZX: {
		// r32, r/m8|16 (zero extension) -> unsigned typed data
		if (insn.op_count() == 2 && IsX86Reg(insn.ops(0)) && IsX86Mem(insn.ops(1)))
			return ArrayOp((uint8_t)insn.ops(1).size, true, ArrayOp::TypedUnsigned);
		return ArrayOp();
	}
	case X86_INS_MOVSX:
	case X86_INS_MOVSXD: {
		if (insn.op_count() == 2 && IsX86Reg(insn.ops(0)) && IsX86Mem(insn.ops(1)))
			return ArrayOp((uint8_t)insn.ops(1).size, true, ArrayOp::TypedSigned);
		return ArrayOp();
	}
	}
	return ArrayOp();
}

std::unique_ptr<ILInstr> FunctionAnalyzer::processLoadStore(AsmIterator& insn)
{
	InsnMarker marker(insn);

	// load/store with fixed offset (object field or array element)
	//   movl/movq val, [obj + disp]      ; load
	//   movq [obj + disp], val           ; store
	if (insn.id() == X86_INS_MOV && insn.op_count() == 2 && IsX86Reg(insn.ops(0)) && IsX86Mem(insn.ops(1))) {
		// load
		const auto& mem = insn.ops(1).mem;
		if (!IsCsDartFp((x86_reg)mem.base) && mem.disp != 0) {
			const auto arrayOp = getArrayOp(insn, true);
			if (!arrayOp.IsArrayOp())
				return nullptr;
			const auto valReg = A64::Register{ insn.ops(0).reg };
			const auto objReg = A64::Register{ mem.base };
			const auto offset = mem.disp;
			++insn;

			if (arrayOp.arrType == ArrayOp::Unknown) {
				// might be array or object. set it as object first.
				return std::make_unique<LoadFieldInstr>(insn.Wrap(marker.Take()), valReg, objReg, offset);
			}
			else {
				// typed array element
				const auto idx = VarStorage::NewSmallImm((offset + dart::kHeapObjectTag - dart::UntaggedTypedData::payload_offset()) / arrayOp.size);
				return std::make_unique<LoadArrayElementInstr>(insn.Wrap(marker.Take()), valReg, objReg, idx, arrayOp);
			}
		}
	}
	else if (insn.id() == X86_INS_MOV && insn.op_count() == 2 && IsX86Mem(insn.ops(0)) && IsX86Reg(insn.ops(1))) {
		// store
		const auto& mem = insn.ops(0).mem;
		if (!IsCsDartFp((x86_reg)mem.base) && mem.disp != 0) {
			const auto arrayOp = getArrayOp(insn, false);
			if (!arrayOp.IsArrayOp())
				return nullptr;
			const auto valReg = A64::Register{ insn.ops(1).reg };
			const auto objReg = A64::Register{ mem.base };
			const auto offset = mem.disp;
			++insn;

			if (arrayOp.arrType == ArrayOp::Unknown) {
				// might be array or object field store
				const auto il_wb = processWriteBarrierInstr(insn);
				if (il_wb) {
					INSN_ASSERT(il_wb->objReg == objReg && il_wb->valReg == valReg);
					if (il_wb->isArray)
						return std::make_unique<StoreArrayElementInstr>(insn.Wrap(marker.Take()), valReg, objReg, VarStorage::NewSmallImm(offset), arrayOp);
					else
						return std::make_unique<StoreFieldInstr>(insn.Wrap(marker.Take()), valReg, objReg, offset);
				}
				return std::make_unique<StoreFieldInstr>(insn.Wrap(marker.Take()), valReg, objReg, offset);
			}
			else {
				// typed array element store (no write barrier for typed data)
				const auto idx = VarStorage::NewSmallImm((offset + dart::kHeapObjectTag - dart::UntaggedTypedData::payload_offset()) / arrayOp.size);
				return std::make_unique<StoreArrayElementInstr>(insn.Wrap(marker.Take()), valReg, objReg, idx, arrayOp);
			}
		}
	}

	// indexed access: tmp = arr + idx*scale ; mov [tmp + disp], ...
	if (insn.id() == X86_INS_LEA && insn.op_count() == 2 && IsX86Reg(insn.ops(0)) && IsX86Mem(insn.ops(1))) {
		const auto& mem = insn.ops(1).mem;
		if (mem.base != X86_REG_INVALID && mem.index != X86_REG_INVALID && mem.disp == 0) {
			const auto tmpReg = A64::Register{ insn.ops(0).reg };
			const auto arrReg = A64::Register{ (x86_reg)mem.base };
			const auto idx = VarStorage(A64::Register{ (x86_reg)mem.index });
			InsnMarker idxmarker(insn);
			++insn;

			if (insn.id() == X86_INS_MOV && insn.op_count() == 2 && IsX86Reg(insn.ops(0)) && IsX86Mem(insn.ops(1))) {
				const auto arrayOp = getArrayOp(insn, true);
				if (!arrayOp.IsArrayOp())
					return nullptr;
				const auto& load_mem = insn.ops(1).mem;
				if (!((x86_reg)load_mem.base == tmpReg))
					return nullptr;
				const auto op0Reg = A64::Register{ insn.ops(0).reg };
				++insn;
				return std::make_unique<LoadArrayElementInstr>(insn.Wrap(idxmarker.Take()), op0Reg, arrReg, idx, arrayOp);
			}
			else if (insn.id() == X86_INS_MOV && insn.op_count() == 2 && IsX86Mem(insn.ops(0)) && IsX86Reg(insn.ops(1))) {
				const auto arrayOp = getArrayOp(insn, false);
				if (!arrayOp.IsArrayOp())
					return nullptr;
				const auto& store_mem = insn.ops(0).mem;
				if (!((x86_reg)store_mem.base == tmpReg))
					return nullptr;
				const auto valReg = A64::Register{ insn.ops(1).reg };
				++insn;
				return std::make_unique<StoreArrayElementInstr>(insn.Wrap(idxmarker.Take()), valReg, arrReg, idx, arrayOp);
			}
		}
	}

	return nullptr;
}


std::unique_ptr<InitAsyncInstr> FunctionAnalyzer::processInitAsyncInstr(AsmIterator& insn)
{
#ifdef HAS_INIT_ASYNC
	// InitAsync cannot be tail jump
	if (insn.id() == X86_INS_CALL && insn.op_count() == 1 && IsX86Imm(insn.ops(0))) {
		const auto fn = app.GetFunction(insn.ops(0).imm);
		if (fn && fn->IsStub()) {
			const auto stub = fn->AsStub();
			auto il = fnInfo->LastIL();
			if (stub->kind == DartStub::InitAsyncStub && il->Kind() == ILInstr::LoadValue) {
				auto ilLoad = reinterpret_cast<LoadValueInstr*>(il);
				INSN_ASSERT(ilLoad->dstReg == A64::Register::RAX);
				auto& item = ilLoad->GetValue();
				DartType* returnType;
				if (item.ValueTypeId() == dart::kNullCid) {
					// Future<Null>
					returnType = app.TypeDb()->FindOrAdd(app.DartFutureCid(), &DartTypeArguments::Null);
				}
				else {
					INSN_ASSERT(item.ValueTypeId() == dart::kTypeArgumentsCid);
					auto typeArg = &(item.Get<VarTypeArgument>()->typeArgs);
					returnType = app.TypeDb()->FindOrAdd(app.DartFutureCid(), typeArg);
				}
				fnInfo->returnType = returnType;
				const auto start = il->Start();
				fnInfo->RemoveLastIL();
				setAsmTextDataCall(insn.address(), (uint64_t)insn.ops(0).imm);
				++insn;
				return std::make_unique<InitAsyncInstr>(insn.Wrap(start), returnType);
			}
		}
	}
#endif
	return nullptr;
}

std::unique_ptr<CallLeafRuntimeInstr> FunctionAnalyzer::processCallLeafRuntime(AsmIterator& insn)
{
	// LeafRuntimeScope::Call() (x64). The exact shape depends on the runtime
	// entry (call_native_through_safepoint_entry_point) and is calibrated against
	// real app.so output. Two common forms:
	//   A) movq TMP, [THR + entry] ; call TMP
	//   B) call [THR + entry]
	InsnMarker marker(insn);

	uint64_t thr_offset = 0;
	if (insn.id() == X86_INS_MOV && insn.op_count() == 2 && IsX86Reg(insn.ops(0)) && IsX86Mem(insn.ops(1)) &&
		(x86_reg)insn.ops(1).mem.base == X86_REG_R14) {
		// possible load of leaf runtime entry point
		const auto tmp_reg = insn.ops(0).reg;
		thr_offset = insn.ops(1).mem.disp;
		++insn;
		if (insn.id() == X86_INS_CALL && insn.op_count() == 1 && IsX86Reg(insn.ops(0)) && insn.ops(0).reg == tmp_reg) {
			++insn;
			return std::make_unique<CallLeafRuntimeInstr>(insn.Wrap(marker.Take()), thr_offset);
		}
		return nullptr;
	}
	else if (insn.id() == X86_INS_CALL && insn.op_count() == 1 && IsX86Mem(insn.ops(0)) &&
		(x86_reg)insn.ops(0).mem.base == X86_REG_R14) {
		thr_offset = insn.ops(0).mem.disp;
		++insn;
		return std::make_unique<CallLeafRuntimeInstr>(insn.Wrap(marker.Take()), thr_offset);
	}

	return nullptr;
}

// ----------------------------------------------------------------------------
// Prologue & parameter handling (x64 AOT: Dart args passed on caller stack)
// ----------------------------------------------------------------------------
FunctionAnalyzer::StoreLocalResult FunctionAnalyzer::handleStoreLocal(AsmIterator& insn, A64::Register expected_src_reg)
{
	// mov [rbp + neg_offset], src  (save value into a local stack slot)
	auto saved_ins = insn.Current();
	int offset = 0;
	A64::Register srcReg;
	if (insn.id() == X86_INS_MOV && insn.op_count() == 2 && IsX86Mem(insn.ops(0)) && IsX86Reg(insn.ops(1))) {
		const auto& mem = insn.ops(0).mem;
		if (IsCsDartFp((x86_reg)mem.base) && mem.disp < 0) {
			if (!expected_src_reg.IsSet() || expected_src_reg == A64::Register{ insn.ops(1).reg }) {
				srcReg = A64::Register{ insn.ops(1).reg };
				offset = (int)mem.disp;
				++insn;
			}
		}
	}
	if (offset == 0)
		insn.SetCurrent(saved_ins);
	return StoreLocalResult{ offset, srcReg };
}

std::tuple<A64::Register, A64::Register> FunctionAnalyzer::unboxParam(AsmIterator& insn, A64::Register expectedSrcReg)
{
	A64::Register srcReg;
	A64::Register dstReg;

	// extract value from Double object: movq dst, [src + Double::value_offset - tag]
	if (insn.id() == X86_INS_MOV && insn.op_count() == 2 && IsX86Reg(insn.ops(0)) && IsX86Mem(insn.ops(1)) &&
		insn.ops(1).mem.disp == AOT_Double_value_offset - dart::kHeapObjectTag) {
		dstReg = A64::Register{ insn.ops(0).reg };
		if (!dstReg.IsDecimal())
			return { A64::Register{}, A64::Register{} };
		srcReg = A64::Register{ insn.ops(1).mem.base };
		if (expectedSrcReg.IsSet() && expectedSrcReg != srcReg)
			return { A64::Register{}, A64::Register{} };
		++insn;
	}
	else {
		auto il = processLoadInt32FromBoxOrSmiInstrFromSrcReg(insn, expectedSrcReg);
		if (!il)
			return { A64::Register{}, A64::Register{} };
		srcReg = il->srcObjReg;
		dstReg = il->dstReg;
	}

	return { dstReg, srcReg };
}

void FunctionAnalyzer::handleFixedParameters(AsmIterator& insn)
{
	// x64 AOT: fixed parameters are read directly from the caller stack,
	// [rbp + 8*(i+2)] (+8 return addr, +8 saved rbp), and usually copied to a
	// local slot at a negative offset from rbp.
	int paramCnt = dartFn->NumParam();
	if (paramCnt == 0)
		paramCnt = INT_MAX;
	for (auto i = 0; i < paramCnt; i++) {
		if (!(insn.id() == X86_INS_MOV && insn.op_count() == 2 && IsX86Reg(insn.ops(0)) && IsX86Mem(insn.ops(1)) &&
			IsCsDartFp((x86_reg)insn.ops(1).mem.base) && insn.ops(1).mem.disp >= 0x10))
			break;
		const auto valReg = A64::Register{ insn.ops(0).reg };
		++insn;

		fnInfo->State()->ClearRegister(valReg);
		auto val = fnInfo->Vars()->ValParam(fnInfo->params.NumParam());
		fnInfo->State()->SetRegister(valReg, val);

		// the parameter value might be saved to stack as a local variable
		const auto storeRes = handleStoreLocal(insn, valReg);
		if (storeRes.fpOffset != 0) {
			fnInfo->State()->SetLocal(storeRes.fpOffset, val);
		}

		fnInfo->params.add(FnParamInfo{ valReg, storeRes.fpOffset });
	}

	fnInfo->params.numFixedParam = fnInfo->params.NumParam();
	if (!dartFn->IsStatic() && fnInfo->params.numFixedParam > 0) {
		// class method. first parameter is "this"
		fnInfo->params[0].name = "this";
		fnInfo->params[0].type = dartFn->Class().DeclarationType();
	}
}

void FunctionAnalyzer::handleOptionalPositionalParameters(AsmIterator& insn)
{
	// Optional positional parameters on x64 AOT are handled with the same
	// ArgumentsDescriptor pattern as arm64, but without writeback/extended
	// addressing. The compiler emits a sequence of:
	//   cmp <count_reg>, #(i+1)<<1 ; jl <defaults_i>
	// followed by loading the passed values from [rbp + off] and the default
	// values from PP / immediates.
	int i = 0;
	while (insn.id() == X86_INS_CMP && insn.op_count() == 2 && IsX86Reg(insn.ops(0)) && IsX86Imm(insn.ops(1))) {
		const auto paramCntReg = insn.ops(0).reg;
		if (insn.ops(1).imm != ((int64_t)(i + 1) << 1))
			break;
		++insn;

		if (!(insn.id() == X86_INS_JL))
			break;
		const auto defaultValueTarget = insn.BranchTarget();
		++insn;

		// parameter might not be used and no loading value
		if (insn.id() == X86_INS_MOV && insn.op_count() == 2 && IsX86Reg(insn.ops(0)) && IsX86Mem(insn.ops(1)) &&
			IsCsDartFp((x86_reg)insn.ops(1).mem.base) && insn.ops(1).mem.disp > 0) {
			const auto valReg = A64::Register{ insn.ops(0).reg };
			fnInfo->State()->ClearRegister(valReg);
			auto val = fnInfo->Vars()->ValParam(fnInfo->params.NumParam());
			fnInfo->State()->SetRegister(valReg, val);
			fnInfo->params.add(FnParamInfo{ valReg });
			++insn;

			const auto storeRes = handleStoreLocal(insn, valReg);
			if (storeRes.fpOffset != 0) {
				fnInfo->State()->SetLocal(storeRes.fpOffset, val);
			}
		}
		else {
			fnInfo->params.add(FnParamInfo{});
		}

		++i;
	}
}

void FunctionAnalyzer::handleOptionalNamedParameters(AsmIterator& insn)
{
	// Named parameters are matched from the ArgumentsDescriptor. x64 keeps the
	// descriptor in R10 (ARGS_DESC_REG). Detection mirrors the arm64 path:
	// values are loaded from [desc_base + disp] with disp >= first_named_entry.
	auto argsDescReg = X86_REG_INVALID;
	if (insn.id() == X86_INS_MOV && insn.op_count() == 2 && IsX86Reg(insn.ops(0)) && IsX86Mem(insn.ops(1))) {
		const auto& mem = insn.ops(1).mem;
		if (IsCsDartFp((x86_reg)mem.base) && mem.disp >= 0 && fnInfo->State()->GetValue((x86_reg)mem.base) == fnInfo->Vars()->ValArgsDesc()) {
			argsDescReg = mem.base;
		}
	}
	// placeholder: full named-parameter analysis requires real app.so samples to
	// pin down the x64 descriptor access shapes.
}

void FunctionAnalyzer::handleArgumentsDescriptorTypeArguments(AsmIterator& insn)
{
	// generic function: type argument count is read from ArgumentsDescriptor
	//   mov typeArgLenReg, [argsdesc + type_args_len_offset - tag]
	// then the type argument vector is loaded from [rbp + off].
	if (insn.id() == X86_INS_MOV && insn.op_count() == 2 && IsX86Reg(insn.ops(0)) && IsX86Mem(insn.ops(1))) {
		const auto typeArgLenReg = insn.ops(0).reg;
		++insn;
		const auto storeRes = handleStoreLocal(insn, A64::Register{ typeArgLenReg });
		if (storeRes.fpOffset != 0) {
			fnInfo->typeArgumentLocalOffset = storeRes.fpOffset;
		}
	}
}

std::unique_ptr<SetupParametersInstr> FunctionAnalyzer::processPrologueParametersInstr(AsmIterator& insn, uint64_t endPrologueAddr)
{
	// PrologueBuilder::BuildPrologue() reverse: fixed params, optional params,
	// closure context handling and type arguments handling.
	InsnMarker marker(insn);

	// initialize synthetic :suspend_state (async / sync* functions)
	if (insn.id() == X86_INS_MOV && insn.op_count() == 2 && IsX86Mem(insn.ops(0)) && IsX86Imm(insn.ops(1)) &&
		IsCsDartFp((x86_reg)insn.ops(0).mem.base) && insn.ops(0).mem.disp == -8 && insn.ops(1).imm == 0) {
		// store null to [rbp - 8]
		++insn;
	}

	// ArgumentsDescriptor (R10) might be saved to the local slot first
	if (insn.id() == X86_INS_MOV && insn.op_count() == 2 && IsX86Mem(insn.ops(0)) && IsX86Reg(insn.ops(1)) &&
		IsCsDartFp((x86_reg)insn.ops(0).mem.base) && insn.ops(1).reg == X86_REG_R10) {
		++insn;
	}

	handleFixedParameters(insn);
	handleOptionalPositionalParameters(insn);
	handleOptionalNamedParameters(insn);
	handleArgumentsDescriptorTypeArguments(insn);

	return std::make_unique<SetupParametersInstr>(insn.Wrap(marker.Take()), &fnInfo->params);
}

void FunctionAnalyzer::handlePrologue(AsmIterator& insn, uint64_t endPrologueAddr)
{
	{
		auto ilEnter = processEnterFrameInstr(insn);
		if (!ilEnter) {
			// no EnterFrame at a beginning of function. likely to be leaf function.
			return;
		}
		fnInfo->AddIL(std::move(ilEnter));
	}

	{
		auto ilAlloc = processAllocateStackInstr(insn);
		if (!ilAlloc) {
			// no local variable (very rare)
			return;
		}
		fnInfo->AddIL(std::move(ilAlloc));
	}

	bool hasPrologue = false;
#ifdef HAS_INIT_ASYNC
	if (fnInfo->stackSize) {
		fnInfo->InitVars();
		fnInfo->InitState();
		try {
			auto il = processPrologueParametersInstr(insn, endPrologueAddr);
			if (il) {
				fnInfo->AddIL(std::move(il));
				hasPrologue = true;
				for (auto& pending_il : fnInfo->Vars()->pending_ils) {
					fnInfo->AddIL(std::move(pending_il));
				}
				fnInfo->Vars()->pending_ils.clear();
			}
		}
		catch (InsnException& e) {
			printInsnException(e);
		}
		fnInfo->DestroyState();
		fnInfo->DestroyVars();
	}
#endif

	if (hasPrologue && endPrologueAddr != 0 && endPrologueAddr != insn.address()) {
		//std::cerr << std::format("endPrologueAddr != insn.address(), {:#x} != {:#x}\n", endPrologueAddr, insn.address());
	}

	auto ilStack = processCheckStackOverflowInstr(insn);
	if (ilStack) {
		fnInfo->AddIL(std::move(ilStack));
	}
}

void FunctionAnalyzer::printInsnException(InsnException& e)
{
	const auto& insn = *e.insn;
	std::cerr << std::format("[{}] condition not matched: {}\n", insn.address, e.cond) << std::endl;
}

void FunctionAnalyzer::asm2il()
{
	AsmIterator insn(asm_insns.FirstPtr(), asm_insns.LastPtr());

	handlePrologue(insn, fnInfo->asmTexts.FirstStackLimitAddress());

	do {
		bool ok = false;
		try {
			for (auto matcher : matcherFns) {
				auto il = std::invoke(matcher, this, insn);
				if (il) {
					fnInfo->AddIL(std::move(il));
					ok = true;
					break;
				}
			}
		}
		catch (InsnException& e) {
			printInsnException(e);
		}

		if (!ok) {
			auto ins = insn.Current();
			fnInfo->AddIL(std::make_unique<UnknownInstr>(ins, fnInfo->asmTexts.AtAddr(ins->address)));
			++insn;
		}
	} while (!insn.IsEnd());
}

void CodeAnalyzer::asm2il(DartFunction* dartFn, AsmInstructions& asm_insns)
{
	FunctionAnalyzer analyzer{ dartFn->GetAnalyzedData(), dartFn, asm_insns, app };
	analyzer.asm2il();
}

static bool renameDartRegToken(char*& op_ptr, char*& out)
{
	if (op_ptr[0] == 'r' && op_ptr[1] == '1' && op_ptr[2] == '5' &&
		(op_ptr[3] == '\0' || op_ptr[3] == ',' || op_ptr[3] == ']' || op_ptr[3] == ' ' || op_ptr[3] == '+')) {
		*out++ = 'P'; *out++ = 'P';
		op_ptr += 3;
		return true;
	}
	if (op_ptr[0] == 'r' && op_ptr[1] == '1' && op_ptr[2] == '4' &&
		(op_ptr[3] == '\0' || op_ptr[3] == ',' || op_ptr[3] == ']' || op_ptr[3] == ' ' || op_ptr[3] == '+')) {
		*out++ = 'T'; *out++ = 'H'; *out++ = 'R';
		op_ptr += 3;
		return true;
	}
	if (op_ptr[0] == 'r' && op_ptr[1] == '1' && op_ptr[2] == '2' &&
		(op_ptr[3] == '\0' || op_ptr[3] == ',' || op_ptr[3] == ']' || op_ptr[3] == ' ' || op_ptr[3] == '+')) {
		*out++ = 'C'; *out++ = 'O'; *out++ = 'D'; *out++ = 'E';
		op_ptr += 3;
		return true;
	}
	if (op_ptr[0] == 'r' && op_ptr[1] == '1' && op_ptr[2] == '1' &&
		(op_ptr[3] == '\0' || op_ptr[3] == ',' || op_ptr[3] == ']' || op_ptr[3] == ' ' || op_ptr[3] == '+')) {
		*out++ = 'T'; *out++ = 'M'; *out++ = 'P';
		op_ptr += 3;
		return true;
	}
	if (op_ptr[0] == 'r' && op_ptr[1] == '1' && op_ptr[2] == '0' &&
		(op_ptr[3] == '\0' || op_ptr[3] == ',' || op_ptr[3] == ']' || op_ptr[3] == ' ' || op_ptr[3] == '+')) {
		*out++ = 'A'; *out++ = 'R'; *out++ = 'G'; *out++ = 'S';
		op_ptr += 3;
		return true;
	}
	if (op_ptr[0] == 'r' && op_ptr[1] == 'b' && op_ptr[2] == 'p' &&
		(op_ptr[3] == '\0' || op_ptr[3] == ',' || op_ptr[3] == ']' || op_ptr[3] == ' ' || op_ptr[3] == '+')) {
		*out++ = 'f'; *out++ = 'p';
		op_ptr += 3;
		return true;
	}
	if (op_ptr[0] == 'r' && op_ptr[1] == 's' && op_ptr[2] == 'p' &&
		(op_ptr[3] == '\0' || op_ptr[3] == ',' || op_ptr[3] == ']' || op_ptr[3] == ' ' || op_ptr[3] == '+')) {
		*out++ = 'S'; *out++ = 'P';
		op_ptr += 3;
		return true;
	}
	return false;
}

AsmTexts CodeAnalyzer::convertAsm(AsmInstructions& asm_insns)
{
	std::vector<AsmText> asm_texts(asm_insns.Count());
	uint64_t first_stack_limit_addr = 0;
	int max_param_stack_offset = 0;

	for (size_t i = 0; i < asm_insns.Count(); i++) {
		auto insn = asm_insns.Ptr(i);
		auto& text_asm = asm_texts.at(i);

		text_asm.addr = insn->address;
		text_asm.dataType = AsmText::None;

		memset(text_asm.text, ' ', 16);
		const auto mlen = strlen(insn->mnemonic);
		memcpy(text_asm.text, insn->mnemonic, mlen > 15 ? 15 : mlen);
		auto ptr = text_asm.text + 16;
		char op_buf[256];
		strncpy(op_buf, insn->op_str, sizeof(op_buf) - 1);
		op_buf[sizeof(op_buf) - 1] = '\0';
		char* op_ptr = op_buf;
		bool token_start = true;
		while (*op_ptr != '\0' && (ptr - text_asm.text) < (int)sizeof(text_asm.text) - 1) {
			if (token_start && renameDartRegToken(op_ptr, ptr)) {
				token_start = false;
				continue;
			}
			switch (*op_ptr) {
			case ' ':
			case '[':
			case ',':
			case '+':
			case '*':
				token_start = true;
				break;
			default:
				token_start = false;
				break;
			}
			*ptr++ = *op_ptr++;
		}
		*ptr = '\0';

		if (!insn->detail)
			continue;

		const auto& x86 = insn->detail->x86;
		for (uint8_t oi = 0; oi < x86.op_count; oi++) {
			const auto& op = x86.operands[oi];
			if (op.type != X86_OP_MEM)
				continue;

			if (IsCsDartPp((x86_reg)op.mem.base)) {
				// PP is tagged on x64: Capstone disp == element_offset - kHeapObjectTag
				text_asm.dataType = AsmText::PoolOffset;
				text_asm.poolOffset = op.mem.disp;
			}
			else if (IsCsDartThr((x86_reg)op.mem.base)) {
				text_asm.dataType = AsmText::ThreadOffset;
				text_asm.threadOffset = op.mem.disp;
				if (first_stack_limit_addr == 0 &&
					op.mem.disp == AOT_Thread_stack_limit_offset) {
					first_stack_limit_addr = insn->address;
				}
			}
			else if (IsCsDartFp((x86_reg)op.mem.base)) {
				if (op.mem.disp > max_param_stack_offset)
					max_param_stack_offset = (int)op.mem.disp;
			}
		}

		if (insn->id == X86_INS_CALL || insn->id == X86_INS_JMP) {
			for (uint8_t oi = 0; oi < x86.op_count; oi++) {
				const auto& op = x86.operands[oi];
				if (op.type == X86_OP_IMM) {
					text_asm.dataType = AsmText::Call;
					text_asm.callAddress = (uint64_t)op.imm;
					break;
				}
			}
		}
	}

	if (asm_texts.empty()) {
		asm_texts.push_back(AsmText{});
	}
	return AsmTexts{ std::move(asm_texts), first_stack_limit_addr, max_param_stack_offset };
}

#endif // NO_CODE_ANALYSIS













