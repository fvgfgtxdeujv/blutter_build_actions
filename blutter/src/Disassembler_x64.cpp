#include "pch.h"
#include "Disassembler.h"

namespace A64 {
const char* Register::RegisterNames[] = {
	"rax", "rcx", "rdx", "rbx", "SP", "fp", "rsi", "rdi",
	"r8", "r9", "ARGS", "TMP", "CODE", "r13", "THR", "PP",
};
}

Disassembler::Disassembler(bool hasDetail)
{
	if (cs_open(CS_ARCH_X86, CS_MODE_64, &cshandle) != CS_ERR_OK)
		throw std::runtime_error("Cannot open capstone engine (x86_64)");

	if (hasDetail)
		cs_option(cshandle, CS_OPT_DETAIL, CS_OPT_ON);
}
