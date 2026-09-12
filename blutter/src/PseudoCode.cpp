#include "pch.h"
#include "PseudoCode.h"
#include "CodeAnalyzer.h"
#include "DartApp.h"
#include "DartFunction.h"
#include "il.h"
#include <utility>
#include <unordered_set>

// ============================================================================
// Pseudo-code view (see PseudoCode.h for the design contract).
//
// Implementation notes:
//   * We merge the IL stream with the asm stream exactly like DartDumper does
//     (IL instructions cover a range of addresses and are printed once when the
//     asm cursor enters their range).
//   * Expressions live in a register->expr map.  Every assignment-style IL/asm
//     updates the map instead of emitting text; a value only shows up when it is
//     *consumed* (store / call-arg / branch / return), which removes most of the
//     mov noise.  To keep v1 honest, registers that are still live at the end of
//     a path are emitted as a trailing `return`/`// live` line.
//   * Raw instructions that do not match any rule are emitted as commented lines
//     `// 0x... <asm>` so no information is hidden.
// ============================================================================

namespace {

using std::string;
using std::vector;
using std::unordered_map;
using std::pair;

bool isRegToken(const string& t)
{
	if (t.empty())
		return false;
	if (t == "TMP" || t == "ARGS" || t == "PP" || t == "THR" || t == "CODE")
		return true;
	if (t.size() >= 2) {
		const char c0 = t[0];
		if (c0 == 'x' || c0 == 'w') {
			// x0..x30 / w0..w30
			if (t.size() <= 3) {
				for (size_t i = 1; i < t.size(); i++)
					if (!isdigit((unsigned char)t[i]))
						return false;
				return true;
			}
			return false;
		}
	}
	// x64 GP registers: rax rcx rdx rbx rsp rbp rsi rdi r8..r15 + 32/16-bit subregs
	const char* kGp[] = {
		"rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp",
		"eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp",
		"ax", "bx", "cx", "dx", "si", "di", "bp", "sp",
		"al", "bl", "cl", "dl", "sil", "dil", "bpl", "spl",
		"r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
		"r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d",
	};
	for (const auto* r : kGp)
		if (t == r)
			return true;
	return false;
}

// Split a Capstone x64 operand text on the top-level commas (no nesting).
vector<string> splitTopLevel(const string& s)
{
	vector<string> parts;
	string cur;
	for (char c : s) {
		if (c == ',') {
			if (!cur.empty())
				parts.push_back(cur);
			cur.clear();
		}
		else {
			cur += c;
		}
	}
	if (!cur.empty())
		parts.push_back(cur);
	return parts;
}

string trimStr(const string& s)
{
	size_t b = s.find_first_not_of(" \t");
	if (b == string::npos)
		return "";
	size_t e = s.find_last_not_of(" \t");
	return s.substr(b, e - b + 1);
}

struct MemRef {
	bool valid{ false };
	string base;   // normalized register name or "PP"/"THR"/"SP"/"fp"/"CODE"
	int64_t disp{ 0 };
	bool hasDisp{ false };
};

// parse "qword ptr [fp - 0x88]" / "[rax + 0x10]" style operand into MemRef
MemRef parseMemRef(const string& raw)
{
	MemRef m;
	auto open = raw.find('[');
	if (open == string::npos)
		return m;
	auto close = raw.find(']', open);
	if (close == string::npos)
		return m;
	string inner = trimStr(raw.substr(open + 1, close - open - 1));
	// inner: "<base> [+/- 0x...]" possibly with an index "* ..." we ignore
	// x64 winapp body loads are plain [base + disp]; keep it simple.
	size_t sp = inner.find_first_of(" \t");
	if (sp == string::npos) {
		m.base = inner;
	}
	else {
		m.base = inner.substr(0, sp);
		string rest = trimStr(inner.substr(sp));
		if (!rest.empty()) {
			// forms: "+ 0x10" / "- 0x8" / "* 4 + 0x18" (indexed: unsupported -> raw)
			if (rest[0] == '+' || rest[0] == '-') {
				char sign = rest[0];
				string num = trimStr(rest.substr(1));
				// capstone hex has no 0x prefix handling issue; parse both
				char* end = nullptr;
				long long v = strtoll(num.c_str(), &end, 0);
				if (end && *end == '\0' && end != num.c_str()) {
					m.disp = sign == '-' ? -v : v;
					m.hasDisp = true;
				}
				else {
					return m; // unparsable disp -> leave invalid-ish (raw fallback)
				}
			}
			else {
				return m; // indexed or scaled form: not handled by v1 folding
			}
		}
	}
	m.valid = true;
	return m;
}

class PseudoGen {
public:
	PseudoGen(const DartFunction& dartFn, AnalyzedFnData& ad, bool isX64,
		const std::function<std::string(intptr_t)>& poolDesc)
		: dartFn_(dartFn), ad_(ad), isX64_(isX64), poolDesc_(poolDesc) {}

	string run()
	{
		auto& asmTexts = ad_.asmTexts.Data();
		auto& il_insns = ad_.il_insns;
		if (asmTexts.empty())
			return "";
		(void)dartFn_; // kept for future use (e.g. static/async hints)

		auto il_itr = il_insns.begin();
		const auto il_end = il_insns.end();

		// --- parameters known from analysis --------------------------------
		// gap-4/x64 & arm64 bind receiver as "this" and the rest arg_N
		const auto& params_ = ad_.params.params;
		const auto numFixed_ = ad_.params.numFixedParam;
		haveParams = !params_.empty();
		params = &params_;
		numFixed = (int)numFixed_;

		for (auto& asmText : asmTexts) {
			// flush IL entries that start before or at this asm address
			while (il_itr != il_end && (*il_itr)->Start() <= asmText.addr) {
				processIL(il_itr->get());
				++il_itr;
			}
			processAsm(asmText);
		}
		// flush any IL after the last asm
		while (il_itr != il_end) {
			processIL(il_itr->get());
			++il_itr;
		}

		// registers left live at the end: surface them as a comment
		if (!emittedReturn_) {
			for (auto& [reg, expr] : regExpr_) {
				(void)reg;
				if (!expr.empty()) {
					// only report the conventional result regs
					if (reg == "r0" || reg == "rax" || reg == "x0")
						line("// live " + reg + " = " + expr);
				}
			}
		}

		string out;
		for (auto& l : lines_) {
			out += l;
			out += '\n';
		}
		return out;
	}

private:
	const DartFunction& dartFn_;
	AnalyzedFnData& ad_;
	bool isX64_;
	// kept for API compatibility; the view deliberately renders pool refs as
	// `[pp+off]` instead of resolving them (see replacePoolRef)
	[[maybe_unused]] const std::function<std::string(intptr_t)>& poolDesc_;

	vector<string> lines_;
	unordered_map<string, string> regExpr_; // register -> expression string
	unordered_map<int64_t, string> fpSlot_; // fp-relative slot (negative) -> expr
	vector<pair<int64_t, string>> spArgs_;  // [SP + off] slots written before a call
	bool swallowNextCond_{ false };         // next jcc belongs to the overflow slow path

	void line(const string& s) { lines_.push_back(s); }

	bool isInternalBase(const string& baseExpr) const
	{
		// Dart VM internal areas: thread/pool/code/static-field-table reads and
		// writes carry no application meaning and are already lifted to IL.
		if (baseExpr == "THR" || baseExpr == "PP" || baseExpr == "CODE")
			return true;
		return baseExpr.rfind("THR", 0) == 0 || baseExpr.rfind("PP", 0) == 0 ||
			baseExpr.rfind("CODE", 0) == 0;
	}

	string replacePoolRef(const string& text, intptr_t /*poolOffset*/)
	{
		// Normalise "[PP + 0x3f]" to "[pp+0x3f]".  We deliberately avoid
		// getPoolObjectDescription() here: it reaches ObjectToString() and the
		// type DB while it may still be filling up, a known crash source on
		// some samples.  The asm reference lines keep the rich description.
		string out = text;
		size_t p = 0;
		while ((p = out.find("[PP", p)) != string::npos) {
			out.replace(p, 3, "[pp");
			p += 3;
		}
		return out;
	}

	string hexAddr(uint64_t a) { return std::format("{:#x}", a); }

	// Drop Capstone size prefixes ("qword ptr " ...) so condition operands read
	// closer to C.  The asm reference lines still carry the exact form.
	static string stripSizePrefix(const string& s)
	{
		string out = s;
		for (const char* pfx : { "qword ptr ", "dword ptr ", "word ptr ", "byte ptr " }) {
			const string p = pfx;
			size_t pos = 0;
			while ((pos = out.find(p, pos)) != string::npos)
				out.erase(pos, p.size());
		}
		return out;
	}

	// ---------------------------------------------------------------------
	// register-name expansion inside a piece of asm text
	// ---------------------------------------------------------------------
	// VarStorage::Name() calls FATAL() for kinds it does not handle
	// (Immediate / Expression / Call / Field / Uninit).  arm64 IL can produce
	// such storage kinds (e.g. an array index held in an expression), and the
	// pseudo view must never abort the whole dump.  Format defensively here.
	static string storageName(const VarStorage& s)
	{
		switch (s.kind) {
		case VarStorage::Register:      return s.reg.Name();
		case VarStorage::Local:         return std::format("local_{:x}", -s.offset);
		case VarStorage::Argument:      return std::format("arg_{}", s.idx);
		case VarStorage::Static:        return std::format("static_{:x}", s.offset);
		case VarStorage::Pool:          return std::format("PP_{:x}", s.offset);
		case VarStorage::Thread:        return std::format("THR_{:x}", s.offset);
		case VarStorage::SmallImm:      return std::to_string(s.offset);
		case VarStorage::InInstruction: return "tmp";
		case VarStorage::Immediate:     return "imm";
		default:                        return "expr";
		}
	}

	static string varItemName(const VarItem& vi)
	{
		VarStorage s = vi.Storage();
		if (s.kind == VarStorage::Immediate) {
			auto* v = vi.Value();
			return v != nullptr ? v->ToString() : string("imm");
		}
		return storageName(s);
	}

	string expandToken(const string& tok)
	{
		if (isRegToken(tok)) {
			auto it = regExpr_.find(tok);
			if (it != regExpr_.end() && !it->second.empty())
				return it->second;
		}
		return tok;
	}

	string expandText(const string& text)
	{
		// word-boundary replace of register tokens by their current expression
		string out = text;
		// build candidates from the map first (longest keys win to avoid partials)
		vector<string> keys;
		for (auto& [k, v] : regExpr_)
			if (!v.empty())
				keys.push_back(k);
		std::sort(keys.begin(), keys.end(), [](const string& a, const string& b) { return a.size() > b.size(); });
		for (auto& k : keys) {
			string needle = k;
			size_t pos = 0;
			while ((pos = out.find(needle, pos)) != string::npos) {
				bool l = pos == 0 || !(isalnum((unsigned char)out[pos - 1]) || out[pos - 1] == '_');
				size_t e = pos + needle.size();
				bool r = e >= out.size() || !(isalnum((unsigned char)out[e]) || out[e] == '_');
				if (l && r) {
					out.replace(pos, needle.size(), regExpr_[k]);
					pos += regExpr_[k].size();
				}
				else {
					pos += needle.size();
				}
			}
		}
		return out;
	}

	// ---------------------------------------------------------------------
	// value plumbing
	// ---------------------------------------------------------------------
	void setReg(const string& reg, const string& expr) { regExpr_[reg] = expr; }

	string readFpSlot(int64_t disp)
	{
		// parameter area starts at fp+0x10 (first arg).  Use analyzed param
		// names only for the fixed prefix we are sure about; map by slot order.
		if (disp >= 0x10) {
			uint64_t idx = (uint64_t)(disp - 0x10) / 8;
			if (haveParams && params != nullptr && idx < (uint64_t)numFixed && idx < params->size()) {
				const auto& p = (*params)[idx];
				if (p.name == "this")
					return "this";
				if (!p.name.empty())
					return p.name;
				return std::format("arg_{}", idx);
			}
			return std::format("arg_{}", idx); // beyond known params: best effort
		}
		auto it = fpSlot_.find(disp);
		if (it != fpSlot_.end() && !it->second.empty())
			return it->second;
		return std::format("fp{:#x}", disp);
	}

	void writeFpSlot(int64_t disp, const string& expr) { fpSlot_[disp] = expr; }

	// [SP + off] argument slots are collected but only flushed at a call.
	void writeSpArg(int64_t off, const string& expr)
	{
		for (auto& [o, v] : spArgs_) {
			if (o == off) {
				v = expr;
				return;
			}
		}
		spArgs_.emplace_back(off, expr);
	}

	// ---------------------------------------------------------------------
	// IL handling (architecture neutral)
	// ---------------------------------------------------------------------
	void processIL(ILInstr* il)
	{
		switch (il->Kind()) {
		case ILInstr::EnterFrame:
		case ILInstr::AllocateStack:
		case ILInstr::LeaveFrame:
		case ILInstr::Unknown:
			return; // boilerplate
		case ILInstr::CheckStackOverflow:
			// the branch right after the overflow check goes to the runtime slow
			// path (rethrow / stack overflow); swallow it as boilerplate
			swallowNextCond_ = true;
			return;
		case ILInstr::SetupParameters:
			return;
		case ILInstr::LoadValue: {
			auto* v = static_cast<LoadValueInstr*>(il);
			setReg(v->dstReg.Name(), varItemName(v->val));
			return;
		}
		case ILInstr::MoveReg: {
			auto* v = static_cast<MoveRegInstr*>(il);
			auto it = regExpr_.find(v->srcReg.Name());
			if (it != regExpr_.end())
				setReg(v->dstReg.Name(), it->second);
			return;
		}
		case ILInstr::LoadField: {
			auto* v = static_cast<LoadFieldInstr*>(il);
			string obj = expandToken(v->objReg.Name());
			if (isInternalBase(obj)) {
				line("// " + il->ToString()); // VM-internal field read, not app data
				regExpr_.erase(v->dstReg.Name());
				return;
			}
			setReg(v->dstReg.Name(), obj + "->field_" + std::format("{:x}", v->offset));
			return;
		}
		case ILInstr::StoreField: {
			auto* v = static_cast<StoreFieldInstr*>(il);
			string obj = expandToken(v->objReg.Name());
			string val = expandToken(v->valReg.Name());
			// `SP->field_8 = x` is actually a stack-arg slot write (x64 call arg)
			if (std::string(v->objReg.Name()) == "SP") {
				writeSpArg((int64_t)v->offset, val);
				return;
			}
			if (isInternalBase(obj)) {
				line("// " + il->ToString()); // VM-internal field write
				return;
			}
			line(std::format("{}->field_{:x} = {}", obj, v->offset, val));
			return;
		}
		case ILInstr::LoadArrayElement: {
			auto* v = static_cast<LoadArrayElementInstr*>(il);
			string arr = expandToken(v->arrReg.Name());
			setReg(v->dstReg.Name(), arr + "[" + expandToken(storageName(v->idx)) + "]");
			return;
		}
		case ILInstr::StoreArrayElement: {
			auto* v = static_cast<StoreArrayElementInstr*>(il);
			string arr = expandToken(v->arrReg.Name());
			string val = expandToken(v->valReg.Name());
			line(arr + "[" + expandToken(storageName(v->idx)) + "] = " + val + ";");
			return;
		}
		case ILInstr::Call: {
			auto* v = static_cast<CallInstr*>(il);
			string name = v->GetFunction() ? v->GetFunction()->Name() : std::format("0x{:x}", v->GetCallAddress());
			emitCall(name);
			return;
		}
		case ILInstr::ClosureCall: {
			auto* v = static_cast<ClosureCallInstr*>(il);
			emitCall(std::format("closure({} args)", v->numArg), "r0");
			return;
		}
		case ILInstr::GdtCall: {
			// no stable target name: keep annotated raw
			line("// " + il->ToString());
			return;
		}
		case ILInstr::CallLeafRuntime:
			line("// runtime: " + il->ToString());
			return;
		case ILInstr::Return: {
			emitReturn();
			return;
		}
		case ILInstr::AllocateObject: {
			auto* v = static_cast<AllocateObjectInstr*>(il);
			setReg(v->dstReg.Name(), "new " + v->dartCls.Name() + "()");
			return;
		}
		case ILInstr::TestType: {
			auto* v = static_cast<TestTypeInstr*>(il);
			string src = expandToken(v->srcReg.Name());
			line(std::format("// {} is {} (TestType)", src, v->typeName));
			return;
		}
		case ILInstr::BoxInt64: {
			auto* v = static_cast<BoxInt64Instr*>(il);
			setReg(v->objReg.Name(), expandToken(v->srcReg.Name()));
			return;
		}
		case ILInstr::LoadInt32: {
			auto* v = static_cast<LoadInt32Instr*>(il);
			setReg(v->dstReg.Name(), expandToken(v->srcObjReg.Name()) + ".toInt32()");
			return;
		}
		case ILInstr::LoadStaticField: {
			auto* v = static_cast<LoadStaticFieldInstr*>(il);
			setReg(v->DstReg().Name(), std::format("static({:#x})", v->FieldOffset()));
			return;
		}
		case ILInstr::StoreStaticField: {
			auto* v = static_cast<StoreStaticFieldInstr*>(il);
			line(std::format("static({:#x}) = {};", v->FieldOffset(), expandToken(v->ValReg().Name())));
			inPrologue_ = false;
			return;
		}
		case ILInstr::InitLateStaticField:
		case ILInstr::WriteBarrier:
		case ILInstr::SaveRegister:
		case ILInstr::RestoreRegister:
		case ILInstr::DecompressPointer:
		case ILInstr::LoadClassId:
		case ILInstr::LoadTaggedClassIdMayBeSmi:
		case ILInstr::BranchIfSmi:
			line("// " + il->ToString());
			return;
		default:
			line("// " + il->ToString());
			return;
		}
	}

	// stack args collected before the call become the argument list
	void emitCall(const string& name, const string& resultReg = "r0")
	{
		std::sort(spArgs_.begin(), spArgs_.end(),
			[](const auto& a, const auto& b) { return a.first < b.first; });
		string args;
		bool first = true;
		for (auto& [off, expr] : spArgs_) {
			if (!first)
				args += ", ";
			first = false;
			args += expr;
		}
		spArgs_.clear();
		if (args.empty())
			args = "..."; // analyzer did not see the args
		lastCallResultReg_ = resultReg;
		lastCallText_ = name + "(" + args + ")";
		setReg(resultReg, lastCallText_);
		inPrologue_ = false;
	}

	void emitReturn()
	{
		if (emittedReturn_)
			return;
		emittedReturn_ = true;
		// result register: x64 rax, arm64 x0 (r0 alias in IL text)
		string result;
		if (!lastCallText_.empty()) {
			result = lastCallText_;
		}
		else {
			for (auto* cand : { "rax", "x0", "r0" }) {
				auto it = regExpr_.find(cand);
				if (it != regExpr_.end() && !it->second.empty()) {
					result = it->second;
					break;
				}
			}
		}
		if (!result.empty())
			line("return " + result + ";");
		else
			line("return;");
	}

	// ---------------------------------------------------------------------
	// asm handling
	// ---------------------------------------------------------------------
	void processAsm(const AsmText& at)
	{
		// split mnemonic (text[0..16)) from operands (text[16..))
		string full(at.text);
		// trim trailing NUL / spaces of the whole 71-byte field is already NUL-terminated
		size_t nul = full.find('\0');
		if (nul != string::npos)
			full.resize(nul);
		full = trimStr(full);
		if (full.empty())
			return;
		size_t sp = full.find_first_of(" \t");
		string mnem = sp == string::npos ? full : full.substr(0, sp);
		string ops = sp == string::npos ? "" : trimStr(full.substr(sp));

		// The analyzer usually lifts the interesting data-flow already; if an IL
		// statement was emitted for this address we only need branch/return notes.
		const bool isTerm = mnem == "ret" || mnem == "retn";
		if (isTerm) {
			if (!emittedReturn_)
				emitReturn();
			inPrologue_ = false;
			return;
		}

		if (!isX64_) {
			// ARM64: only annotate control flow; everything else was lifted to IL
			// or is left as raw line.
			if (mnem[0] == 'b' || mnem == "cbz" || mnem == "cbnz" || mnem == "tbz" || mnem == "tbnz")
				line(std::format("// 0x{:x}: {} (branch) -> {}", at.addr, mnem, ops));
			else if (at.dataType == AsmText::PoolOffset)
				line(std::format("// 0x{:x}: [pp+{:#x}]", at.addr, at.poolOffset));
			return;
		}

		// ---------------- x64 folding ----------------
		if (mnem == "mov") {
			processX64Mov(at, ops);
			return;
		}
		if (mnem == "jmp") {
			line(std::format("// goto {}", ops));
			inPrologue_ = false;
			return;
		}
		if (isCondJmp(mnem)) {
			if (swallowNextCond_) {
				// overflow-check branch: runtime slow path, no value to a reader
				swallowNextCond_ = false;
				return;
			}
			string cond;
			if (!pendingCond_.empty()) {
				cond = " if (" + buildCondExpr(mnem) + ")";
			}
			line(std::format("//{} goto {}", cond, ops));
			pendingCond_.clear();
			inPrologue_ = false;
			return;
		}
		if (mnem == "cmp" || mnem == "test") {
			// remember the expanded operands for the next conditional jump
			pendingCond_ = expandText(ops);
			if (at.dataType == AsmText::PoolOffset)
				pendingCond_ = replacePoolRef(pendingCond_, at.poolOffset);
			pendingCond_ = stripSizePrefix(pendingCond_);
			lastCondIsTest_ = mnem == "test";
			return;
		}
		if (mnem == "call") {
			if (at.dataType == AsmText::Call)
				emitCall(std::format("0x{:x}", at.callAddress));
			else
				line(std::format("// call {}", expandText(ops))); // indirect call
			return;
		}
		// whatever remains: keep a raw reference line (info never lost)
		if (!inPrologue_ && isMeaningfulRaw(mnem))
			line(std::format("// {} {}", mnem, expandText(ops)));
	}

	bool isCondJmp(const string& m)
	{
		static const char* k[] = { "je", "jne", "ja", "jae", "jb", "jbe", "jg", "jge", "jl", "jle", "js", "jns", "jz", "jnz", "jo", "jno", "jp", "jnp" };
		for (auto* c : k)
			if (m == c)
				return true;
		return false;
	}

	// Build `lhs OP rhs` from the pending cmp/test and the jcc mnemonic.
	// cmp a, b sets flags for a-b: je -> a == b, jne -> a != b, etc.
	// test a, b sets flags for a&b:  je -> a&b == 0 (commonly test reg,reg -> reg==0)
	string buildCondExpr(const string& jcc)
	{
		auto parts = splitTopLevel(pendingCond_);
		string lhs = parts.empty() ? "?" : trimStr(parts[0]);
		string rhs = parts.size() > 1 ? trimStr(parts[1]) : "";
		if (lastCondIsTest_) {
			if (rhs.empty() || rhs == lhs) {
				// test r, r: flags from r itself; je iff r == 0
				const char* op = (jcc == "je" || jcc == "jz") ? "==" : (jcc == "jne" || jcc == "jnz") ? "!=" : "?";
				if (string(op) == "?")
					return lhs + " ?(test)";
				return lhs + " " + op + " 0";
			}
			return lhs + " & " + rhs + " ?(test)";
		}
		// cmp: signed/unsigned idioms approximated with the common equality set
		string op;
		if (jcc == "je" || jcc == "jz")
			op = "==";
		else if (jcc == "jne" || jcc == "jnz")
			op = "!=";
		else if (jcc == "ja" || jcc == "jg")
			op = ">";
		else if (jcc == "jae" || jcc == "jge")
			op = ">=";
		else if (jcc == "jb" || jcc == "jl")
			op = "<";
		else if (jcc == "jbe" || jcc == "jle")
			op = "<=";
		else
			return lhs + " ?(cmp)";
		return lhs + " " + op + " " + rhs;
	}

	// x64 Capstone Intel mov parsing.  Handles (all with optional size prefix):
	//   mov reg, reg | mov reg, imm | mov reg, [mem] | mov [mem], reg
	// where [mem] is [base +/- 0x..] with base in fp/SP/PP/THR/CODE or a reg.
	void processX64Mov([[maybe_unused]] const AsmText& at, const string& opsRaw)
	{
		auto parts = splitTopLevel(opsRaw);
		if (parts.size() < 2) {
			line(std::format("// mov {}", opsRaw));
			return;
		}
		string dst = trimStr(parts[0]);
		string src = trimStr(parts[1]);
		// normalize "qword ptr [..]" prefixes
		auto stripPtrPrefix = [](string& t) {
			size_t p = t.find("ptr");
			if (p != string::npos && t.find('[') != string::npos) {
				size_t open = t.find('[');
				t = trimStr(t.substr(open));
			}
		};
		stripPtrPrefix(dst);
		stripPtrPrefix(src);

		if ((dst == "fp" && src == "SP") || (dst == "SP" && src == "fp")) {
			// frame setup / teardown boilerplate
			return;
		}

		if (dst[0] == '[') {
			// memory store
			MemRef m = parseMemRef(dst);
			if (!m.valid) {
				line(std::format("// mov {} = {}", dst, src));
				return;
			}
			string val = expandToken(src);
			if (m.base == "fp")
				writeFpSlot(m.disp, val);
			else if (m.base == "SP")
				writeSpArg(m.disp, val);
			else {
				string bexpr = expandToken(m.base);
				if (isInternalBase(bexpr))
					return; // field-table / pool / thread internal writes (IL covers them)
				line(std::format("{}[{}] = {}", bexpr, hexOffset(m.disp), val));
			}
			inPrologue_ = false;
			return;
		}
		// register / immediate load
		if (!isRegToken(dst)) {
			line(std::format("// mov {} = {}", dst, src));
			return;
		}
		string val;
		if (src[0] == '[') {
			MemRef m = parseMemRef(src);
			if (m.valid) {
				if (m.base == "fp") {
					val = readFpSlot(m.disp);
				}
				else if (m.base == "PP") {
					// pool constant: keep the raw offset form.  Resolving it via
					// getPoolObjectDescription() would reach ObjectToString() and
					// the (still filling) type DB too early, which is a known
					// crash source on some samples; the asm comment keeps the
					// rich description anyway.
					val = std::format("[pp{}]", hexOffset(m.disp));
				}
				else if (m.base == "THR") {
					// thread-internal read (null / stack limit / dispatch tbl):
					// no application value, keep the enclosing expression as IL
					return;
				}
				else if (m.base == "CODE")
					val = std::format("CODE{}", hexOffset(m.disp));
				else {
					string bexpr = expandToken(m.base);
					if (isInternalBase(bexpr))
						return; // implementation detail of an IL-lifted access
					if (m.disp == -1)
						val = bexpr + "->[-1]"; // class-id / header tag reads
					else
						val = bexpr + std::format("[{}]", hexOffset(m.disp));
				}
			}
			else {
				// unparsable memory form: raw
				line(std::format("// mov {} = {}", dst, src));
				return;
			}
		}
		else {
			val = expandToken(src);
		}
		setReg(dst, val);
		inPrologue_ = false;
	}

	string hexOffset(int64_t off)
	{
		if (off < 0)
			return std::format("-{:#x}", -off);
		if (off == 0)
			return "0";
		return std::format("+{:#x}", off);
	}

	bool isMeaningfulRaw(const string& m)
	{
		static const char* kNoise[] = {
			"nop", "push", "pop", "endbr64", "movdqa", "movdqu",
		};
		for (auto* n : kNoise)
			if (m == n)
				return false;
		// arithmetic that only sets flags for a following branch was handled by
		// cmp/test; but add/sub/and/lea often compute real values -> keep raw
		return true;
	}

	string lastCallResultReg_{ "r0" };
	string lastCallText_;
	bool haveParams{ false };
	bool emittedReturn_{ false };
	bool inPrologue_{ true };
	string pendingCond_;
	bool lastCondIsTest_{ false };
	const vector<FnParamInfo>* params{ nullptr };
	int numFixed{ 0 };
};

} // namespace

namespace PseudoCode {

string Generate(const DartFunction& dartFn,
	AnalyzedFnData& analyzedData,
	bool isX64,
	const std::function<std::string(intptr_t)>& poolDesc)
{
	PseudoGen gen(dartFn, analyzedData, isX64, poolDesc);
	return gen.run();
}

} // namespace PseudoCode
