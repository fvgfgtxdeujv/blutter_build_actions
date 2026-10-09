#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// Control-flow structuring for the pseudo-code view
// (see .monkeycode/specs/2026-10-06-dart-control-flow-structuring).
//
// The existing pseudo view is a purely linear fold of the asm/IL stream.  This
// module adds one layer on top of the *same* asm text: it splits the function
// into basic blocks, builds the intra-procedural CFG, finds natural loops via
// back edges, and classifies each block with a small structural "role".
// PseudoCode then renders the structured `if/else`, `while`, `do/while` and
// `switch` lines and falls back to the legacy `// if (...) goto 0x...`
// annotation for any region it cannot structure.
//
// It never touches the classic asm/*.dart output: it is only consulted while
// the -p/--pseudo view is being generated.

struct AsmText;

namespace ControlFlow {

struct BasicBlock {
	uint64_t begin{ 0 };
	uint64_t end{ 0 }; // exclusive (address right after the last instruction)
	uint64_t last{ 0 }; // address of the terminator instruction (last in block)
	std::vector<uint64_t> succ;
	std::vector<uint64_t> pred;

	enum TermKind { Fall, Jump, CondJump, Ret, Unknown };
	TermKind term{ Unknown };

	// Raw (unfolded) compare operand text feeding this block's conditional
	// branch, plus the normalised condition suffix ("eq", "ne", ...).  Empty
	// when the terminator is not a conditional branch or no compare precedes it.
	std::string cmpLhs;
	std::string cmpRhs;
	std::string cond;
	bool cmpIsTest{ false };

	uint64_t target{ 0 };     // taken target of Jump / CondJump
	uint64_t fallTarget{ 0 }; // fallthrough of CondJump / Fall
	bool targetExternal{ false };

	int loopDepth{ 0 };
	bool irreducible{ false };
};

struct LoopInfo {
	uint64_t header{ 0 };
	uint64_t tail{ 0 }; // back-edge source
	std::vector<uint64_t> body; // block begins, includes header
	bool preTest{ true }; // true => while, false => do/while
};

struct Cfg {
	std::vector<BasicBlock> blocks; // ascending address
	std::unordered_map<uint64_t, size_t> addrToIdx;
	std::vector<LoopInfo> loops;

	const BasicBlock* BlockAt(uint64_t addr) const
	{
		auto it = addrToIdx.find(addr);
		return it == addrToIdx.end() ? nullptr : &blocks[it->second];
	}
};

// Structural roles attached to a block boundary.  Rendering (with the folded
// condition text produced by the existing pseudo folder) happens in PseudoCode.
enum class BeginKind {
	None,
	DoOpen,       // emit "do {"
	SwitchOpen,   // emit "switch (<expr>) {"
	CaseLabel,    // emit "case <value>:"
	DefaultLabel, // emit "default:"
};

enum class TermKind {
	None,
	IfOpen,       // emit "if (cond) {" (or negated) after the branch
	IfElseOpen,   // emit "} else {"
	WhileOpen,    // emit "while (cond) {" (or negated) after the header branch
	DoWhileClose, // emit "} while (cond);"
	TermClose,    // emit "}"
};

struct StructRole {
	BeginKind beginKind{ BeginKind::None };
	TermKind termKind{ TermKind::None };
	int beginClose{ 0 }; // number of '}' to emit at the block begin
	bool negateCond{ false };
	bool suppressTermGoto{ false };
	// Set once a block has been consumed by any structuring pass, so later
	// passes never re-structure the same block (e.g. switch chain blocks).
	bool handled{ false };
	std::string caseValue;
	std::string switchExpr;
};

struct BuildOptions {
	bool isX64{ false };
	// IL-provided branch targets keyed by the branch instruction address
	// (BranchIfSmi / CheckStackOverflow).  Takes precedence over the operand.
	std::unordered_map<uint64_t, uint64_t> ilBranchTargets;
	// Guard against pathological CFGs.
	size_t maxBlocks{ 65536 };
};

// Build the CFG (blocks + edges) and detect natural loops.
Cfg Build(const std::vector<AsmText>& asmTexts, const BuildOptions& opts);

// Classify blocks into structural roles.  index-aligned with cfg.blocks.
// Regions that cannot be structured are left with BeginKind/TermKind = None so
// the caller keeps the legacy goto annotation.
std::vector<StructRole> Structure(const Cfg& cfg);

} // namespace ControlFlow
