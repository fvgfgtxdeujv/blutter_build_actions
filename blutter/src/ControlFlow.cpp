#include "pch.h"
#include "ControlFlow.h"
#include "CodeAnalyzer.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <deque>
#include <set>

namespace {

using std::string;
using std::vector;

string trimC(const string& s)
{
	size_t b = s.find_first_not_of(" \t\r\n");
	if (b == string::npos)
		return "";
	size_t e = s.find_last_not_of(" \t\r\n");
	return s.substr(b, e - b + 1);
}

vector<string> splitTop(const string& s)
{
	vector<string> parts;
	string cur;
	int depth = 0;
	for (char c : s) {
		if (c == '[')
			depth++;
		else if (c == ']') {
			if (depth > 0)
				depth--;
		}
		if (c == ',' && depth == 0) {
			parts.push_back(trimC(cur));
			cur.clear();
		}
		else {
			cur += c;
		}
	}
	if (!cur.empty() || !parts.empty())
		parts.push_back(trimC(cur));
	return parts;
}

bool parseAddr(const string& tok, uint64_t& out)
{
	string t = trimC(tok);
	if (t.empty())
		return false;
	if (t[0] == '#')
		t = t.substr(1);
	char* end = nullptr;
	unsigned long long v = strtoull(t.c_str(), &end, 0);
	if (end == nullptr || end == t.c_str() || *end != '\0')
		return false;
	out = (uint64_t)v;
	return true;
}

bool looksLikeReg(const string& t)
{
	if (t.empty())
		return false;
	if (t[0] == '[')
		return false;
	return isalpha((unsigned char)t[0]) || t[0] == '_';
}

bool isX64CondJmp(const string& m)
{
	static const char* k[] = { "je", "jne", "ja", "jae", "jb", "jbe", "jg", "jge",
		"jl", "jle", "js", "jns", "jz", "jnz", "jo", "jno", "jp", "jnp" };
	for (auto* c : k)
		if (m == c)
			return true;
	return false;
}

struct Instr {
	uint64_t addr{ 0 };
	string mnem;
	string ops;
};

string asmFull(const AsmText& at)
{
	string full(at.text);
	size_t nul = full.find('\0');
	if (nul != string::npos)
		full.resize(nul);
	return trimC(full);
}

Instr parseInstr(const AsmText& at)
{
	Instr in;
	in.addr = at.addr;
	const string full = asmFull(at);
	size_t sp = full.find_first_of(" \t");
	in.mnem = sp == string::npos ? full : full.substr(0, sp);
	in.ops = sp == string::npos ? "" : trimC(full.substr(sp));
	return in;
}

// find the last compare-like instruction in [first,last] (inclusive indices)
void findCompare(const vector<Instr>& ins, size_t first, size_t last, ControlFlow::BasicBlock& b)
{
	for (size_t k = last; k + 1 > first && k >= first && k != (size_t)-1; k--) {
		const auto& in = ins[k];
		if (in.mnem == "cmp" || in.mnem == "cmn" || in.mnem == "test" || in.mnem == "tst") {
			auto parts = splitTop(in.ops);
			if (parts.size() >= 1)
				b.cmpLhs = parts[0];
			if (parts.size() >= 2)
				b.cmpRhs = parts[1];
			b.cmpIsTest = (in.mnem == "test" || in.mnem == "tst");
			return;
		}
		if (k == 0)
			break;
	}
}

} // namespace

namespace ControlFlow {

Cfg Build(const std::vector<AsmText>& asmTexts, const BuildOptions& opts)
{
	Cfg cfg;
	if (asmTexts.empty())
		return cfg;

	vector<Instr> ins;
	ins.reserve(asmTexts.size());
	for (auto& at : asmTexts)
		ins.push_back(parseInstr(at));

	const uint64_t firstAddr = ins.front().addr;
	const uint64_t lastAddr = ins.back().addr;

	// ---- boundaries -------------------------------------------------------
	std::set<uint64_t> bounds;
	bounds.insert(firstAddr);

	// determine whether an instruction is a terminator and its taken target
	auto branchTarget = [&](const Instr& in, bool& isTerm, bool& isCond, uint64_t& target) {
		isTerm = false;
		isCond = false;
		target = 0;
		if (!opts.isX64) {
			if (in.mnem == "ret" || in.mnem == "br") {
				isTerm = true;
				return;
			}
			if (in.mnem == "b") {
				isTerm = true;
				if (parseAddr(in.ops, target))
					return;
				return;
			}
			if (in.mnem.size() > 2 && in.mnem[0] == 'b' && in.mnem[1] == '.') {
				isTerm = true;
				isCond = true;
				parseAddr(in.ops, target);
				return;
			}
			if (in.mnem == "cbz" || in.mnem == "cbnz") {
				auto p = splitTop(in.ops);
				if (p.size() >= 2) {
					isTerm = true;
					isCond = true;
					parseAddr(p[1], target);
				}
				return;
			}
			if (in.mnem == "tbz" || in.mnem == "tbnz") {
				auto p = splitTop(in.ops);
				if (p.size() >= 3) {
					isTerm = true;
					isCond = true;
					parseAddr(p[2], target);
				}
				return;
			}
			return;
		}
		// x64
		if (in.mnem == "ret" || in.mnem == "retn") {
			isTerm = true;
			return;
		}
		if (in.mnem == "jmp") {
			isTerm = true;
			parseAddr(in.ops, target); // register/memory target -> parse fails
			return;
		}
		if (isX64CondJmp(in.mnem)) {
			isTerm = true;
			isCond = true;
			parseAddr(in.ops, target);
			return;
		}
	};

	for (size_t i = 0; i < ins.size(); i++) {
		bool isTerm = false, isCond = false;
		uint64_t target = 0;
		branchTarget(ins[i], isTerm, isCond, target);
		// IL-known branch target takes precedence over the parsed operand.
		auto ov = opts.ilBranchTargets.find(ins[i].addr);
		if (ov != opts.ilBranchTargets.end())
			target = ov->second;
		if (isTerm) {
			if (target >= firstAddr && target <= lastAddr)
				bounds.insert(target);
			if (i + 1 < ins.size())
				bounds.insert(ins[i + 1].addr); // fallthrough
		}
	}

	vector<uint64_t> bnd(bounds.begin(), bounds.end());
	if (bnd.size() > opts.maxBlocks)
		return cfg; // pathological: leave unstructured

	std::unordered_map<uint64_t, size_t> addrToInstr;
	for (size_t i = 0; i < ins.size(); i++)
		addrToInstr[ins[i].addr] = i;

	// ---- blocks -----------------------------------------------------------
	cfg.blocks.reserve(bnd.size());
	for (size_t bi = 0; bi < bnd.size(); bi++) {
		BasicBlock b;
		b.begin = bnd[bi];
		b.end = (bi + 1 < bnd.size()) ? bnd[bi + 1] : (lastAddr + 1);

		auto it = addrToInstr.find(b.begin);
		if (it == addrToInstr.end())
			continue;
		size_t first = it->second;
		size_t last = first;
		while (last + 1 < ins.size() && ins[last + 1].addr < b.end)
			last++;

		const Instr& term = ins[last];
		b.last = term.addr;
		b.term = BasicBlock::Fall;
		b.fallTarget = b.end;

		bool isTerm = false, isCond = false;
		uint64_t target = 0;
		branchTarget(term, isTerm, isCond, target);
		auto ov = opts.ilBranchTargets.find(term.addr);
		if (ov != opts.ilBranchTargets.end())
			target = ov->second;

		if (isTerm && term.mnem != "br" && !(opts.isX64 && term.mnem == "jmp" && looksLikeReg(trimC(term.ops)))) {
			if (isCond) {
				b.term = BasicBlock::CondJump;
				b.target = target;
				b.cond = term.mnem;
				if (!opts.isX64)
					b.cond = (term.mnem[0] == 'b' && term.mnem[1] == '.') ? term.mnem.substr(2) : term.mnem;
				else
					b.cond = term.mnem.substr(1);
				findCompare(ins, first, last, b);
				// cbz/tbz feed an explicit compare operand
				if (!opts.isX64 && (term.mnem == "cbz" || term.mnem == "cbnz")) {
					auto p = splitTop(term.ops);
					if (!p.empty()) {
						b.cmpLhs = p[0];
						b.cmpRhs = "0";
						b.cmpIsTest = false;
					}
					b.cond = (term.mnem == "cbz") ? "eq" : "ne";
				}
				else if (!opts.isX64 && (term.mnem == "tbz" || term.mnem == "tbnz")) {
					auto p = splitTop(term.ops);
					if (p.size() >= 2) {
						b.cmpLhs = p[0];
						b.cmpRhs = p[1];
						b.cmpIsTest = false;
					}
					b.cond = (term.mnem == "tbz") ? "eq" : "ne";
				}
			}
			else if (term.mnem == "ret" || term.mnem == "retn") {
				b.term = BasicBlock::Ret;
			}
			else {
				b.term = BasicBlock::Jump;
				b.target = target;
			}
		}

		cfg.blocks.push_back(std::move(b));
	}

	for (size_t i = 0; i < cfg.blocks.size(); i++)
		cfg.addrToIdx[cfg.blocks[i].begin] = i;

	// ---- edges ------------------------------------------------------------
	auto addSucc = [&](size_t i, uint64_t to) {
		auto it = cfg.addrToIdx.find(to);
		if (it == cfg.addrToIdx.end())
			return;
		if (to == cfg.blocks[i].begin)
			return; // ignore self edge here (handled by loop detection)
		cfg.blocks[i].succ.push_back(to);
	};

	for (size_t i = 0; i < cfg.blocks.size(); i++) {
		auto& b = cfg.blocks[i];
		switch (b.term) {
		case BasicBlock::Ret:
			break;
		case BasicBlock::Jump:
			if (cfg.addrToIdx.count(b.target))
				addSucc(i, b.target);
			else
				b.targetExternal = true;
			break;
		case BasicBlock::CondJump:
			if (cfg.addrToIdx.count(b.target))
				addSucc(i, b.target);
			else
				b.targetExternal = true;
			if (b.fallTarget <= lastAddr && cfg.addrToIdx.count(b.fallTarget))
				addSucc(i, b.fallTarget);
			break;
		case BasicBlock::Fall:
		case BasicBlock::Unknown:
		default:
			if (b.fallTarget <= lastAddr && cfg.addrToIdx.count(b.fallTarget))
				addSucc(i, b.fallTarget);
			break;
		}
	}

	for (size_t i = 0; i < cfg.blocks.size(); i++)
		for (auto s : cfg.blocks[i].succ)
			cfg.blocks[cfg.addrToIdx[s]].pred.push_back(cfg.blocks[i].begin);

	// ---- DFS: back edges -> natural loops ---------------------------------
	const int n = (int)cfg.blocks.size();
	vector<int> entry(n, -1), exit_(n, -1);
	int timer = 0;
	{
		vector<char> visited(n, 0);
		for (int s = 0; s < n; s++) {
			if (visited[s])
				continue;
			struct Frame { int node; size_t si; };
			vector<Frame> st;
			st.push_back({ s, 0 });
			visited[s] = 1;
			entry[s] = timer++;
			while (!st.empty()) {
				Frame& f = st.back();
				auto& succ = cfg.blocks[f.node].succ;
				if (f.si < succ.size()) {
					uint64_t sa = succ[f.si++];
					int v = (int)cfg.addrToIdx[sa];
					if (visited[v])
						continue;
					visited[v] = 1;
					entry[v] = timer++;
					st.push_back({ v, 0 });
				}
				else {
					exit_[f.node] = timer++;
					st.pop_back();
				}
			}
		}
	}
	auto isAncestor = [&](int v, int u) {
		return entry[v] <= entry[u] && exit_[v] >= exit_[u] && v != u;
	};

	std::set<int> loopHeaders;
	for (int u = 0; u < n; u++) {
		for (auto sa : cfg.blocks[u].succ) {
			int v = (int)cfg.addrToIdx[sa];
			if (!isAncestor(v, u))
				continue;
			// back edge u -> v: natural loop of header v
			std::set<int> body;
			body.insert(v);
			vector<int> work;
			work.push_back(u);
			while (!work.empty()) {
				int x = work.back();
				work.pop_back();
				if (body.count(x))
					continue;
				body.insert(x);
				for (auto pa : cfg.blocks[x].pred) {
					int p = (int)cfg.addrToIdx[pa];
					if (p != v && !body.count(p))
						work.push_back(p);
				}
			}
			// merge with an existing loop of the same header
			auto existing = loopHeaders.find(v);
			if (existing != loopHeaders.end()) {
				auto& l = cfg.loops[*existing];
				for (int x : body)
					l.body.push_back(cfg.blocks[x].begin);
				continue;
			}
			LoopInfo l;
			l.header = cfg.blocks[v].begin;
			l.tail = cfg.blocks[u].begin;
			for (int x : body)
				l.body.push_back(cfg.blocks[x].begin);
			loopHeaders.insert(v);
			cfg.loops.push_back(std::move(l));
		}
	}

	// de-dup loop bodies + preTest + nesting depth
	for (auto& l : cfg.loops) {
		std::sort(l.body.begin(), l.body.end());
		l.body.erase(std::unique(l.body.begin(), l.body.end()), l.body.end());
		// preTest (while) iff the back edge is unconditional; a conditional back
		// edge is a bottom-tested do/while.
		auto ti = cfg.addrToIdx.find(l.tail);
		l.preTest = ti != cfg.addrToIdx.end() &&
			cfg.blocks[ti->second].term != BasicBlock::CondJump;
	}
	for (auto& b : cfg.blocks) {
		for (auto& l : cfg.loops) {
			if (std::find(l.body.begin(), l.body.end(), b.begin) != l.body.end())
				b.loopDepth++;
		}
	}

	return cfg;
}

std::vector<StructRole> Structure(const Cfg& cfg)
{
	const int n = (int)cfg.blocks.size();
	vector<StructRole> roles(n);
	if (n == 0)
		return roles;

	auto idxOf = [&](uint64_t addr) -> int {
		auto it = cfg.addrToIdx.find(addr);
		return it == cfg.addrToIdx.end() ? -1 : (int)it->second;
	};

	vector<char> reachCache(n, 0);
	vector<vector<char>> reachSets(n);
	auto reach = [&](int s) -> const vector<char>& {
		if (reachCache[s])
			return reachSets[s];
		vector<char> r(n, 0);
		std::deque<int> q;
		q.push_back(s);
		r[s] = 1;
		while (!q.empty()) {
			int x = q.front();
			q.pop_front();
			for (auto sa : cfg.blocks[x].succ) {
				int v = idxOf(sa);
				if (v >= 0 && !r[v]) {
					r[v] = 1;
					q.push_back(v);
				}
			}
		}
		reachCache[s] = 1;
		reachSets[s] = std::move(r);
		return reachSets[s];
	};

	// ---- loops ------------------------------------------------------------
	// self-loops: a single block whose conditional branch targets itself is a
	// compact `do { ... } while (cond)`; an unconditional self jump is `do {}`.
	for (int i = 0; i < n; i++) {
		const BasicBlock& b = cfg.blocks[i];
		if (b.term == BasicBlock::CondJump && b.target == b.begin) {
			roles[i].beginKind = BeginKind::DoOpen;
			roles[i].termKind = TermKind::DoWhileClose;
			roles[i].negateCond = false; // continue when the branch is taken
			roles[i].suppressTermGoto = true;
		}
		else if (b.term == BasicBlock::Jump && b.target == b.begin) {
			roles[i].beginKind = BeginKind::DoOpen;
			roles[i].termKind = TermKind::TermClose;
			roles[i].suppressTermGoto = true;
		}
	}

	// while vs do/while is decided by the *back edge* itself: a conditional
	// back edge (`b.cond header`) is a bottom-tested do/while, while an
	// unconditional back edge (`b header`) means the test sits at the header
	// (top-tested while).  Header predecessors are unreliable here because the
	// function entry into a while header is not an explicit CFG edge.
	for (auto& l : cfg.loops) {
		int h = idxOf(l.header);
		int t = idxOf(l.tail);
		if (h < 0 || t < 0 || h == t)
			continue;
		if (roles[t].termKind != TermKind::None)
			continue; // a block already closes another construct
		const BasicBlock& hb = cfg.blocks[h];
		const BasicBlock& tb = cfg.blocks[t];
		const bool tailCond = (tb.term == BasicBlock::CondJump);
		if (tailCond) {
			// do/while: header opens the body, tail carries the condition
			if (roles[h].beginKind != BeginKind::None)
				continue;
			roles[h].beginKind = BeginKind::DoOpen;
			roles[t].termKind = TermKind::DoWhileClose;
			roles[t].negateCond = false; // continue when the back edge is taken
			roles[t].suppressTermGoto = true;
		}
		else {
			// while: header test branch opens, unconditional tail closes
			if (roles[h].termKind != TermKind::None)
				continue;
			if (hb.term == BasicBlock::CondJump) {
				const bool takenIn = std::find(l.body.begin(), l.body.end(), hb.target) != l.body.end();
				const bool fallIn = std::find(l.body.begin(), l.body.end(), hb.fallTarget) != l.body.end();
				if (takenIn == fallIn)
					continue; // no clear loop-continue branch at the header
				roles[h].termKind = TermKind::WhileOpen;
				roles[h].negateCond = fallIn; // body is fallthrough => while (!cond)
				roles[h].suppressTermGoto = true;
			}
			else if (hb.term == BasicBlock::Fall || hb.term == BasicBlock::Jump) {
				roles[h].termKind = TermKind::WhileOpen; // while (true)
				roles[h].negateCond = false;
				roles[h].suppressTermGoto = true;
			}
			else {
				continue;
			}
			roles[t].termKind = TermKind::TermClose;
			roles[t].suppressTermGoto = true;
		}
	}

	// ---- switch: equality compare chain on one operand --------------------
	for (int i = 0; i < n; i++) {
		if (roles[i].handled || roles[i].termKind != TermKind::None ||
			roles[i].beginKind != BeginKind::None)
			continue;
		const BasicBlock& b0 = cfg.blocks[i];
		if (b0.term != BasicBlock::CondJump || b0.targetExternal)
			continue;
		if (b0.cmpLhs.empty() || b0.cmpRhs.empty())
			continue;
		if (!(b0.cond == "eq" || b0.cond == "e" || b0.cond == "ne" || b0.cond == "z" || b0.cond == "nz"))
			continue;

		vector<int> chain;
		std::set<string> seenVals;
		int cur = i;
		bool ok = true;
		while (cur >= 0) {
			const BasicBlock& cb = cfg.blocks[cur];
			if (cb.term != BasicBlock::CondJump || cb.targetExternal)
				break;
			if (cb.cmpLhs != b0.cmpLhs)
				break;
			if (!(cb.cond == b0.cond))
				break;
			if (!seenVals.insert(cb.cmpRhs).second) {
				ok = false;
				break;
			}
			chain.push_back(cur);
			int fi = idxOf(cb.fallTarget);
			if (fi < 0)
				break;
			const BasicBlock& fb = cfg.blocks[fi];
			// continue the chain only if the fallthrough is another equality
			// compare on the same operand
			if (fb.term == BasicBlock::CondJump && !fb.targetExternal &&
				fb.cmpLhs == b0.cmpLhs && fb.cond == b0.cond)
				cur = fi;
			else
				break;
		}
		if (!ok || chain.size() < 2)
			continue;

		// all involved blocks must be untouched
		bool conflict = false;
		vector<int> targets;
		for (int k : chain) {
			if (roles[k].handled || roles[k].termKind != TermKind::None ||
				roles[k].beginKind != BeginKind::None || roles[k].beginClose > 0)
				conflict = true;
			int ti = idxOf(cfg.blocks[k].target);
			if (ti < 0) {
				conflict = true;
				continue;
			}
			targets.push_back(ti);
		}
		int defaultIdx = idxOf(cfg.blocks[chain.back()].fallTarget);
		if (conflict || defaultIdx < 0)
			continue;
		for (int ti : targets)
			if (roles[ti].handled || roles[ti].beginKind != BeginKind::None ||
				roles[ti].termKind != TermKind::None)
				conflict = true;
		if (roles[defaultIdx].handled || roles[defaultIdx].beginKind != BeginKind::None ||
			roles[defaultIdx].termKind != TermKind::None)
			conflict = true;
		if (conflict)
			continue;

		// common join of every case + default
		vector<char> inter(n, 1);
		bool any = false;
		for (int t : targets) {
			const auto& r = reach(t);
			for (int x = 0; x < n; x++)
				inter[x] = inter[x] && r[x];
			any = true;
		}
		{
			const auto& r = reach(defaultIdx);
			for (int x = 0; x < n; x++)
				inter[x] = inter[x] && r[x];
			any = true;
		}
		int join = -1;
		for (int x = 0; x < n; x++) {
			if (x == i || std::find(chain.begin(), chain.end(), x) != chain.end())
				continue;
			if (inter[x]) {
				join = x;
				break;
			}
		}
		if (!any || join < 0)
			continue;
		if (roles[join].handled || roles[join].beginClose > 0 ||
			roles[join].termKind != TermKind::None || roles[join].beginKind != BeginKind::None)
			continue;

		roles[i].beginKind = BeginKind::SwitchOpen;
		roles[i].switchExpr = b0.cmpLhs;
		for (int k : chain) {
			roles[k].suppressTermGoto = true;
			roles[k].handled = true; // consumed but carries no visible role
		}
		for (size_t ci = 0; ci < chain.size(); ci++) {
			int ti = idxOf(cfg.blocks[chain[ci]].target);
			roles[ti].beginKind = BeginKind::CaseLabel;
			roles[ti].caseValue = cfg.blocks[chain[ci]].cmpRhs;
		}
		roles[defaultIdx].beginKind = BeginKind::DefaultLabel;
		roles[join].beginClose++;
	}

	// ---- if / else --------------------------------------------------------
	for (int i = 0; i < n; i++) {
		if (roles[i].handled || roles[i].termKind != TermKind::None)
			continue;
		const BasicBlock& b = cfg.blocks[i];
		if (b.term != BasicBlock::CondJump || b.targetExternal)
			continue;
		int ti = idxOf(b.target);
		int fi = idxOf(b.fallTarget);
		if (ti < 0 || fi < 0 || ti == fi)
			continue;

		const auto& rt = reach(ti);
		const auto& rf = reach(fi);
		int join = -1;
		for (int x = 0; x < n; x++) {
			if (x == i)
				continue;
			if (rt[x] && rf[x]) {
				join = x;
				break;
			}
		}
		if (join < 0)
			continue;
		// never close a branch into a block that is already owned by a loop or
		// switch boundary (would produce mis-nested braces)
		if (roles[join].termKind != TermKind::None ||
			roles[join].beginKind != BeginKind::None)
			continue;

		if (join == ti) {
			// taken jumps past the body to the join; body is the fallthrough
			roles[i].termKind = TermKind::IfOpen;
			roles[i].negateCond = true;
			roles[i].suppressTermGoto = true;
			roles[join].beginClose++;
		}
		else if (join == fi) {
			roles[i].termKind = TermKind::IfOpen;
			roles[i].negateCond = false;
			roles[i].suppressTermGoto = true;
			roles[join].beginClose++;
		}
		else {
			// canonical if/else: fallthrough body ends with `jmp join`
			const BasicBlock& fb = cfg.blocks[fi];
			if (fb.term == BasicBlock::Jump && idxOf(fb.target) == join) {
				roles[i].termKind = TermKind::IfOpen;
				roles[i].negateCond = true; // branch to else when cond true
				roles[i].suppressTermGoto = true;
				roles[fi].termKind = TermKind::IfElseOpen;
				roles[fi].suppressTermGoto = true;
				roles[join].beginClose++;
			}
			else {
				continue; // do not risk unbalanced braces
			}
		}
	}

	return roles;
}

} // namespace ControlFlow
