#pragma once
#include "Disassembler.h"

// Dart x64 register aliases live under A64 namespace so CodeAnalyzer.h / il.h stay shared.

namespace A64 {

class alignas(int32_t) Register {
public:
	enum Value : int32_t {
		RAX = 0, RCX = 1, RDX = 2, RBX = 3,
		RSP = 4, RBP = 5, RSI = 6, RDI = 7,
		R8 = 8, R9 = 9, R10 = 10, R11 = 11,
		R12 = 12, R13 = 13, R14 = 14, R15 = 15,
		kNumberOfRegisters = 16,
		kNoRegister = -1,
		TMP = R11,
		TMP2 = kNoRegister,
		FP = RBP,
		LR = kNoRegister,
		SP = RSP,
		THR = R14,
		PP = R15,
		CODE = R12,
		ARGS_DESC = R10,
	};

	constexpr Register() : reg(kNoRegister) {}
	constexpr Register(Value reg) : reg(reg) {}
	constexpr Register(dart::Register r) {
		// dart::Register values on x64 match RAX..R15 (0..15)
		const int v = static_cast<int>(r);
		reg = (v >= 0 && v < kNumberOfRegisters) ? static_cast<Value>(v) : kNoRegister;
	}
	constexpr Register(x86_reg r) {
		switch (r) {
		case X86_REG_RAX: case X86_REG_EAX: case X86_REG_AX: case X86_REG_AL: reg = RAX; break;
		case X86_REG_RCX: case X86_REG_ECX: case X86_REG_CX: case X86_REG_CL: reg = RCX; break;
		case X86_REG_RDX: case X86_REG_EDX: case X86_REG_DX: case X86_REG_DL: reg = RDX; break;
		case X86_REG_RBX: case X86_REG_EBX: case X86_REG_BX: case X86_REG_BL: reg = RBX; break;
		case X86_REG_RSP: case X86_REG_ESP: case X86_REG_SP: case X86_REG_SPL: reg = RSP; break;
		case X86_REG_RBP: case X86_REG_EBP: case X86_REG_BP: case X86_REG_BPL: reg = RBP; break;
		case X86_REG_RSI: case X86_REG_ESI: case X86_REG_SI: case X86_REG_SIL: reg = RSI; break;
		case X86_REG_RDI: case X86_REG_EDI: case X86_REG_DI: case X86_REG_DIL: reg = RDI; break;
		case X86_REG_R8:  case X86_REG_R8D:  case X86_REG_R8W:  case X86_REG_R8B:  reg = R8; break;
		case X86_REG_R9:  case X86_REG_R9D:  case X86_REG_R9W:  case X86_REG_R9B:  reg = R9; break;
		case X86_REG_R10: case X86_REG_R10D: case X86_REG_R10W: case X86_REG_R10B: reg = R10; break;
		case X86_REG_R11: case X86_REG_R11D: case X86_REG_R11W: case X86_REG_R11B: reg = R11; break;
		case X86_REG_R12: case X86_REG_R12D: case X86_REG_R12W: case X86_REG_R12B: reg = R12; break;
		case X86_REG_R13: case X86_REG_R13D: case X86_REG_R13W: case X86_REG_R13B: reg = R13; break;
		case X86_REG_R14: case X86_REG_R14D: case X86_REG_R14W: case X86_REG_R14B: reg = R14; break;
		case X86_REG_R15: case X86_REG_R15D: case X86_REG_R15W: case X86_REG_R15B: reg = R15; break;
		default: reg = kNoRegister; break;
		}
	}

	constexpr bool operator==(Register a) const { return reg == a.reg; }
	constexpr bool operator!=(Register a) const { return reg != a.reg; }
	constexpr bool operator==(Value v) const { return reg == v; }
	constexpr bool operator!=(Value v) const { return reg != v; }

	inline bool Clear() { return reg = kNoRegister; }
	inline bool IsSet() const { return reg != kNoRegister; }
	inline bool IsDecimal() const { return false; }

	static const char* RegisterNames[];

	inline const char* Name() const {
		if (reg < 0 || reg >= kNumberOfRegisters)
			return "r?";
		return RegisterNames[reg];
	}

	constexpr operator int() const { return reg; }
	constexpr Value value() const { return reg; }

private:
	Value reg;
};

constexpr auto ARGS_DESC_REG = Register{ Register::ARGS_DESC };
constexpr auto SP_REG = Register{ Register::RSP };
constexpr auto TMP_REG = Register{ Register::TMP };
constexpr auto TMP2_REG = Register{};
constexpr auto NULL_REG = Register{};
constexpr auto THR_REG = Register{ Register::THR };
constexpr auto PP_REG = Register{ Register::PP };

} // namespace A64

constexpr x86_reg CSREG_DART_THR = X86_REG_R14;
constexpr x86_reg CSREG_DART_PP = X86_REG_R15;
constexpr x86_reg CSREG_DART_FP = X86_REG_RBP;
constexpr x86_reg CSREG_DART_SP = X86_REG_RSP;
constexpr x86_reg CSREG_DART_TMP = X86_REG_R11;
constexpr x86_reg CSREG_ARGS_DESC = X86_REG_R10;

inline constexpr bool IsCsDartThr(x86_reg r) {
	return r == X86_REG_R14 || r == X86_REG_R14D || r == X86_REG_R14W || r == X86_REG_R14B;
}
inline constexpr bool IsCsDartPp(x86_reg r) {
	return r == X86_REG_R15 || r == X86_REG_R15D || r == X86_REG_R15W || r == X86_REG_R15B;
}
inline constexpr bool IsCsDartFp(x86_reg r) {
	return r == X86_REG_RBP || r == X86_REG_EBP || r == X86_REG_BP || r == X86_REG_BPL;
}

class AsmInstruction {
private:
	cs_insn* insn;
public:
	class Operands {
		cs_x86_op* operands;
	public:
		Operands(cs_x86_op* operands = nullptr) : operands(operands) {}
		const cs_x86_op& operator[](size_t idx) const { return operands[idx]; }
		const cs_x86_op& operator()(size_t idx) const { return operands[idx]; }
	} ops;

	AsmInstruction(cs_insn* insn)
		: insn(insn),
		  ops(insn && insn->detail ? insn->detail->x86.operands : nullptr) {}
	AsmInstruction& operator=(const AsmInstruction&) = default;

	AsmInstruction& operator++() {
		++insn;
		ops = Operands(insn && insn->detail ? insn->detail->x86.operands : nullptr);
		return *this;
	}
	AsmInstruction& operator--() {
		--insn;
		ops = Operands(insn && insn->detail ? insn->detail->x86.operands : nullptr);
		return *this;
	}
	AsmInstruction& operator+=(int cnt) {
		insn += cnt;
		ops = Operands(insn && insn->detail ? insn->detail->x86.operands : nullptr);
		return *this;
	}
	AsmInstruction Next() { return AsmInstruction(insn + 1); }
	AsmInstruction Prev() { return AsmInstruction(insn - 1); }

	cs_insn* ptr() { return insn; }
	uint64_t address() const { return insn->address; }
	uint16_t size() const { return insn->size; }
	uint64_t NextAddress() const { return insn->address + insn->size; }
	unsigned int id() const { return insn->id; }
	uint8_t op_count() const { return insn->detail ? insn->detail->x86.op_count : 0; }
	const char* mnemonic() const { return insn->mnemonic; }
	const char* op_str() const { return insn->op_str; }
};

struct AddrRange {
	uint64_t start{ 0 };
	uint64_t end{ 0 };

	AddrRange() = default;
	AddrRange(uint64_t start, uint64_t end) : start{ start }, end{ end } {}

	bool Has(uint64_t addr) const { return addr >= start && addr < end; }
};
