#include "pch.h"
#include "CodeAnalyzer.h"
#include "DartApp.h"
#include "DartThreadInfo.h"

#ifndef NO_CODE_ANALYSIS

// Step 04 MVP: annotate PP/THR/call sites for asm dump + IDA.
// Full IL lifting (arm64 FunctionAnalyzer port) is intentionally deferred.

void CodeAnalyzer::asm2il(DartFunction* /*dartFn*/, AsmInstructions& /*asm_insns*/)
{
	// No IL yet on x64 — DumpCode tolerates empty il_insns.
}

static bool renameDartRegToken(char*& op_ptr, char*& out)
{
	// Capstone x86-64 typically prints "r14", "r15", "rbp", "rsp", "r11", "r12", "r10"
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

		// Absolute/relative calls: annotate target if it lands in known Dart code
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
		// Avoid AsmTexts ctor crash on empty (should not happen for Size()>0)
		asm_texts.push_back(AsmText{});
	}
	return AsmTexts{ std::move(asm_texts), first_stack_limit_addr, max_param_stack_offset };
}

#endif // NO_CODE_ANALYSIS
