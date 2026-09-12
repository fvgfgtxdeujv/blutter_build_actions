#pragma once
#include <functional>
#include <string>

struct AnalyzedFnData;
class DartFunction;

// Pseudo-code view generator (official blutter TODO "Some pseudo code for code
// pattern").  Feed it the linear IL + asm texts of one analyzed function and it
// emits a best-effort Dart-ish statement stream:
//   * entry/exit boilerplate (EnterFrame/AllocStack/CheckStackOverflow/...) is dropped
//   * register data-flow is folded into expressions (mov reg/reg, [fp] slots, [SP] args)
//   * field/array loads and stores become `obj->field_x = v;`-style statements
//   * call sites take their stack arguments and fold `return f(...)` when direct
//   * conditional/unconditional jumps become `// if (...) goto 0x...` annotations
// Nothing here claims decompiler-grade control flow: it is a "semantic stream"
// view with raw instructions kept as fallback lines, so information is never lost.
//
// It is a pure incremental view: the classic asm/*.dart output is untouched.
namespace PseudoCode {

// poolDesc: resolver for an object-pool offset to a short human description.
// Kept in the interface but not invoked by the current pass: calling
// DartDumper::getPoolObjectDescription() would force lazy type/object
// materialization this best-effort view does not need, so pool slots are
// rendered as a plain [pp+off] reference instead.
// isX64 selects the asm-level mov/jcc folding rules (x64 Capstone text
// layout); ARM64 keeps IL-level folding only and falls back to annotated raw
// lines for instructions the analyzer did not lift.
std::string Generate(const DartFunction& dartFn,
	AnalyzedFnData& analyzedData,
	bool isX64,
	const std::function<std::string(intptr_t)>& poolDesc);

} // namespace PseudoCode
