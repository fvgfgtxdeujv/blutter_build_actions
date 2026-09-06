#include "pch.h"
#include "DartDumper.h"
#include <fstream>
#include <format>
#include <set>
#include <ranges>
#include <iostream>
#include <sstream>
#include <numeric>
#include <cctype>
#include <string_view>
#include <unordered_set>
#include "Disassembler.h"
#include "DartThreadInfo.h"
#include "CodeAnalyzer.h"
#include <cstdlib>  // std::getenv
#include <fstream>   // blacklist file loader

// TODO: move arm64 specific code to *_arm64 file

// ---------------------------------------------------------------------------
// Semantic-clue blacklists.
//
// Built-in defaults below are used when no blacklist file is given (or the file
// cannot be read). A file overrides them entirely -- edit
//   blutter/src/semantic_blacklist.txt   (or pass --blacklist <file>)
// to add entries without recompiling. Format, one entry per line:
//   # comment / blank line ignored
//   call:<cls::method full name>      -> CALL_BLACKLIST
//   type:<type name>                  -> TYPE_BLACKLIST
//   name:<quoted-string token>        -> NAME_BLACKLIST (function-name candidates)
static const std::vector<std::string> DEFAULT_CALL_BLACKLIST = {
	"$obfuscated::__unknown_function__",
	"$obfuscated::_ffi_resolver_function",
	"Shader::Shader._",
	"LateError::_throwFieldAlreadyInitialized",
	"Native::_ffi_resolver_function",
	"_fw::call",                    // async/stream plumbing wrapper (zip sample)
	"_dw::call",                    // same wrapper pattern (winapp sample)
	"scheduleMicrotask",            // dart:async scheduler, no business value
	"_SecureFilterImpl::buffers",   // dart:io secure-socket internals
	"_SocketControlMessageImpl::level",
	"allocateOneByteString",        // dart runtime string internals
	"_AsyncStarStreamController::addStream",
	"_AsyncStarStreamController::add",
	"_StreamController::Am",
	"_Future::timeout",
	"_Completer::Bod",
};
static const std::vector<std::string> DEFAULT_TYPE_BLACKLIST = {
	"String", "List", "bool", "Object", "void",
};

// Quoted-string literals that must not become a function name. They stay in the
// // semantic: comment and the cross-ref, but naming a function fn_id / fn_dart_ui
// adds no information. Lower-case exact matches only; generic JSON/dart words.
static const std::vector<std::string> DEFAULT_NAME_BLACKLIST = {
	"dart_ui", "id", "type", "name", "method", "text", "code", "key", "data",
	"url", "from", "new", "start", "length", "uri", "free", "value", "root",
	"shell", "display", "error", "number", "list", "map", "size", "index",
	"item", "count", "string", "bool", "call", "class", "null", "other",
	"target", "create", "object", "get", "set", "is", "this", "context",
	"comment", "payload",
};

struct SemanticBlacklists {
	std::unordered_set<std::string> call, type, name;
};

// ---------------------------------------------------------------------------
// Semantic-clue blacklist loader.
//
// The three categories (call/type/name) live in one optional text file so new
// entries can be added without recompiling. Loading happens once on first
// access -- always after main's CLI parsing. Resolution priority:
//   --blacklist <file>  >  $BLUTTER_BLACKLIST  >  compile-time default
//   (BLUTTER_DEFAULT_BLACKLIST_FILE)  >  built-in defaults above.
// A readable file fully replaces the built-ins (entries can also be removed).

static std::string g_blacklistFile; // explicit --blacklist override

void SetSemanticBlacklistFile(const std::string& path)
{
	g_blacklistFile = path;
}

static SemanticBlacklists loadSemanticBlacklists()
{
	SemanticBlacklists bl;
	const auto seed = [](std::unordered_set<std::string>& dst, const std::vector<std::string>& src) {
		dst.insert(src.begin(), src.end());
	};
	seed(bl.call, DEFAULT_CALL_BLACKLIST);
	seed(bl.type, DEFAULT_TYPE_BLACKLIST);
	seed(bl.name, DEFAULT_NAME_BLACKLIST);

	std::string candidate = g_blacklistFile;
	if (candidate.empty()) {
		if (const char* env = std::getenv("BLUTTER_BLACKLIST"))
			candidate = env;
	}
	if (candidate.empty()) {
#ifdef BLUTTER_DEFAULT_BLACKLIST_FILE
		candidate = BLUTTER_DEFAULT_BLACKLIST_FILE;
#endif
	}
	if (candidate.empty())
		return bl;

	std::ifstream ifs(candidate);
	if (!ifs)
		return bl; // file missing: keep built-in defaults

	SemanticBlacklists fileBl;
	std::string line;
	while (std::getline(ifs, line)) {
		if (line.empty() || line[0] == '#')
			continue;
		if (line.starts_with("call:"))
			fileBl.call.insert(line.substr(5));
		else if (line.starts_with("type:"))
			fileBl.type.insert(line.substr(5));
		else if (line.starts_with("name:"))
			fileBl.name.insert(line.substr(5));
		// unknown prefix: ignored (allows loose notes in the file)
	}
	// a file with zero parsed entries means everything was commented out;
	// still replace defaults so removed built-ins actually disappear
	return (fileBl.call.empty() && fileBl.type.empty() && fileBl.name.empty())
		? bl : std::move(fileBl);
}

static const SemanticBlacklists& semanticBlacklists()
{
	static const SemanticBlacklists bl = loadSemanticBlacklists();
	return bl;
}

// Quoted dart string (from getQuoteString) is a semantic clue when it looks
// like an identifier, URL, SQL, error message, or camelCase token — skip
// single-char / punctuation-only / numeric-only literals / hex blobs.
static bool isHexBlob(std::string_view inner)
{
	// a-f count as hex, not as vowels. curve names (secp256k1) have non-hex letters.
	if (inner.size() < 16)
		return false;
	for (size_t i = 0; i < inner.size(); ++i) {
		unsigned char c = (unsigned char)inner[i];
		if (c == '\\' || !std::isxdigit(c))
			return false;
	}
	return true;
}

static bool isSemanticString(const std::string& quoted)
{
	if (quoted.size() < 4) // "x"  -> too short
		return false;
	const char* s = quoted.c_str();
	size_t n = quoted.size();
	if (s[0] != '"' || s[n - 1] != '"')
		return false;
	s++;
	n -= 2;
	if (n < 2 || n > 80)
		return false;
	if (isHexBlob(std::string_view(s, n)))
		return false;

	int alpha = 0, digit = 0, other = 0;
	bool hasSpace = false;
	for (size_t i = 0; i < n; ++i) {
		unsigned char c = (unsigned char)s[i];
		if (c == '\\') {
			++i;
			other++;
			continue;
		}
		if (std::isalpha(c) || c == '_')
			alpha++;
		else if (std::isdigit(c))
			digit++;
		else if (c == ' ')
			hasSpace = true;
		else
			other++;
	}
	if (alpha < 2)
		return false;
	// numeric-heavy (hashes) are not useful as function names
	if (digit > alpha * 2)
		return false;
	// punctuation-only wrappers
	if (other > alpha && !hasSpace)
		return false;
	return true;
}

// Field / Type / Function short names that look like identifiers, not obfuscated junk.
static bool isUsefulIdent(std::string_view name)
{
	if (name.size() < 3 || name.size() > 48)
		return false;
	if (name == "<anonymous closure>" || name == "[no name]" || name == "[unknown]")
		return false;

	// strip leading '_' for the short-junk check: _adg / Teb / _VTl
	std::string_view core = name;
	while (!core.empty() && core.front() == '_')
		core.remove_prefix(1);
	if (core.size() <= 3)
		return false;

	int alpha = 0, digit = 0, other = 0, vowel = 0;
	for (unsigned char c : name) {
		if (c == '_')
			continue;
		else if (std::isalpha(c)) {
			alpha++;
			char lc = (char)std::tolower(c);
			if (lc == 'a' || lc == 'e' || lc == 'i' || lc == 'o' || lc == 'u')
				vowel++;
		}
		else if (std::isdigit(c))
			digit++;
		else
			other++;
	}
	if (other)
		return false;
	if (alpha < 2)
		return false;
	if (!vowel)
		return false;
	if (digit > alpha * 2)
		return false;
	return true;
}

static std::string fieldClueFromCString(const char* raw)
{
	if (!raw)
		return {};
	// "Field <ClassName.fieldName>: late ..."  or  "Field <_GrowableList@0150898._Vm@0150898>: ..."
	std::string_view s(raw);
	auto lt = s.find('<');
	auto gt = s.find('>');
	if (lt == std::string_view::npos || gt == std::string_view::npos || gt <= lt)
		return {};
	auto inner = s.substr(lt + 1, gt - lt - 1);
	auto dot = inner.rfind('.');
	std::string_view shortName = (dot == std::string_view::npos) ? inner : inner.substr(dot + 1);
	auto at = shortName.find('@');
	if (at != std::string_view::npos)
		shortName = shortName.substr(0, at);
	if (!isUsefulIdent(shortName))
		return {};
	return "field:" + std::string(shortName);
}

static std::string typeClueFromName(std::string_view typeName)
{
	// drop type args: SecureSocket / List<int> -> SecureSocket / List
	auto lt = typeName.find('<');
	if (lt != std::string_view::npos)
		typeName = typeName.substr(0, lt);
	if (!typeName.empty() && typeName.back() == '?')
		typeName = typeName.substr(0, typeName.size() - 1);
	if (!isUsefulIdent(typeName))
		return {};
	if (semanticBlacklists().type.count(std::string(typeName)))
		return {};
	return "type:" + std::string(typeName);
}

static std::string callClueFromFn(DartFnBase* fn)
{
	if (!fn || fn->IsStub())
		return {};
	std::string full = fn->FullName();
	// "[dart:io] _ExternalBuffer::start"  or stub-less DartFunction FullName
	std::string_view sv(full);
	auto rb = sv.find(']');
	std::string_view rest = (rb != std::string_view::npos && rb + 2 <= sv.size())
		? sv.substr(rb + 2)
		: sv;
	auto sep = rest.rfind("::");
	std::string_view method = (sep == std::string_view::npos) ? rest : rest.substr(sep + 2);
	std::string_view cls;
	if (sep != std::string_view::npos)
		cls = rest.substr(0, sep);
	if (method == cls)
		return {}; // constructor, low value
	if (method == "<anonymous closure>" || method.empty())
		return {};
	// dart:core (StringBase::_interpolate etc.) is too generic
	if (rb != std::string_view::npos && rb > 1) {
		auto url = sv.substr(1, rb - 1);
		if (url == "dart:core")
			return {};
	}
	if (!method.empty() && method.front() == '_')
		return {};
	// private-class tear-off: _Foo::bar.tearoff
	if (!cls.empty() && cls.front() == '_' && method.find('.') != std::string_view::npos)
		return {};
	if (!isUsefulIdent(method) && !(cls.size() && isUsefulIdent(cls)))
		return {};
	std::string shortName = cls.empty()
		? std::string(method)
		: std::string(cls) + "::" + std::string(method);
	if (semanticBlacklists().call.count(shortName))
		return {};
	return "call:" + shortName;
}

// ---------------------------------------------------------------------------
// Semantic function renaming (see .monkeycode/specs/2026-09-03-ida-semantic-fn-rename)
//
// SDK / package libraries carry a "dart:..." / "package:..." url; business code
// in obfuscated apps has no colon in the url ("Hip", "$obfuscated"). Functions of
// SDK libraries keep their real names and are never renamed.
static bool isSdkLib(const DartLibrary* lib)
{
	return lib && lib->url.find(':') != std::string::npos;
}

// Dart obfuscator short names: "_hFk", "Snb", "Teb", "AAp". Strip leading '_',
// then the core must be short and contain an upper-case letter or a digit.
// Real all-lowercase method names ("load", "add") and longer names never match.
static bool isObfuscatedFnName(const std::string& name)
{
	if (name == "__unknown_function__" || name == "_ffi_resolver_function")
		return true;
	std::string_view core(name);
	while (!core.empty() && core.front() == '_')
		core.remove_prefix(1);
	if (core.empty() || core.size() > 4)
		return false;
	for (unsigned char c : core) {
		if (std::isdigit(c) || std::isupper(c))
			return true;
	}
	return false;
}

static bool isIdentLike(std::string_view s)
{
	if (s.empty())
		return false;
	if (!(std::isalpha((unsigned char)s[0]) || s[0] == '_'))
		return false;
	for (unsigned char c : s) {
		if (!(std::isalnum(c) || c == '_'))
			return false;
	}
	return true;
}

// A quoted-string literal is usable as a function name only when it looks like a
// business identifier: no spaces, lower-case initial (camelCase / snake_case),
// readable word shape (isUsefulIdent), reasonable length, not a generic word.
// Sentence-like error messages, library urls, and random obfuscator strings are
// rejected -- they remain available as // semantic: clues but never as names.
static bool isBusinessToken(std::string_view inner)
{
	if (!isIdentLike(inner) || inner.size() < 3 || inner.size() > 28)
		return false;
	if (!(inner[0] >= 'a' && inner[0] <= 'z'))
		return false;
	if (semanticBlacklists().name.count(std::string(inner)))
		return false;
	return isUsefulIdent(inner);
}

static std::string_view unquoteClue(std::string_view q)
{
	if (q.size() >= 2 && q.front() == '"' && q.back() == '"')
		return q.substr(1, q.size() - 2);
	return q;
}

// Turn arbitrary clue text into an IDA-legal identifier token: non [A-Za-z0-9_]
// becomes '_', runs collapse, edges trimmed, length capped. Empty result when the
// text had no usable character. Leading digit is fine -- callers emit "fn_"+token.
static std::string sanitizeToToken(std::string_view s)
{
	std::string out;
	out.reserve(std::min<size_t>(s.size(), 80));
	char prev = 0;
	for (unsigned char c : s) {
		char t = (std::isalnum(c) || c == '_') ? (char)c : '_';
		if (t == '_' && prev == '_')
			continue;
		out.push_back(t);
		prev = t;
	}
	size_t b = out.find_first_not_of('_');
	if (b == std::string::npos)
		return {};
	size_t e = out.find_last_not_of('_');
	out = out.substr(b, e - b + 1);
	if (out.size() > 64)
		out.resize(64);
	return out;
}

// Pick one readable token for a function from its collected clues. Business-like
// string literals (isBusinessToken) win -- the LAST one in reference order is
// kept, as it usually sits closest to the actual operation (a startVpn trigger
// after the ip/port fields it configures). Call clues are the fallback (method
// segment of "call:Class::method"). Empty result => keep the legacy name.
static std::string genSemanticFnToken(const std::vector<std::string>& strings,
									  const std::vector<std::string>& calls)
{
	std::string best;
	for (auto& q : strings) {
		auto inner = unquoteClue(q);
		if (isBusinessToken(inner))
			best = sanitizeToToken(inner);
	}
	if (!best.empty())
		return best;
	for (auto& c : calls) {
		std::string_view sv(c);
		if (sv.starts_with("call:"))
			sv.remove_prefix(5);
		auto sep = sv.rfind("::");
		if (sep != std::string_view::npos)
			sv = sv.substr(sep + 2);
		auto t = sanitizeToToken(sv);
		if (t.empty() || t.size() < 3 || semanticBlacklists().name.count(t) || !isUsefulIdent(t))
			continue;
		return t;
	}
	return {};
}

static std::unordered_map<std::string, std::string> OP_MAP {
	{ "==", "eq" },
	{ "<", "lt" }, { ">", "gt" },
	{ "<=", "lte" }, { ">=", "gte" },
	{ "=", "assign" },
	{ "[]", "at" }, { "[]=", "at_assign" },
	{ "++", "increment" }, { "--", "decrement" },
	{ "+", "add" }, { "-", "sub" }, { "*", "mul" }, { "~/", "div" }, { "/", "divf" },
	{ "%", "mod" },
	{ "&", "LAnd" }, { "|", "LOr" }, { "^", "xor" }, { "~", "not" }, {">>", "shar"}, {"<<", "shal"}, {">>", "shr"}
};

static std::string getFunctionName4Ida(const DartFunction& dartFn, const std::string& cls_prefix)
{
	auto fnName = dartFn.Name();
	if (dartFn.IsClosure() && fnName == "<anonymous closure>") {
		return "_anon_closure";
	}

	auto periodPos = fnName.find('.');
	std::string prefix;
	if (dartFn.IsStatic() && dartFn.Kind() == DartFunction::NORMAL && periodPos != std::string::npos) {
		// this one is extension
		prefix = fnName.substr(0, periodPos + 1);
		if (prefix.starts_with("_extension#")) {
			// anonymous extension. '#' is invalid name in IDA.
			std::replace(prefix.begin(), prefix.end(), '#', '@');
		}
		fnName = fnName.substr(periodPos + 1);
	}

	if (OP_MAP.contains(fnName)) {
		return prefix + "op_" + OP_MAP[fnName];
	}
	const auto last = fnName.back();
	if (last == '=') {
		fnName.pop_back();
		return prefix + fnName + "_assign";
	}
	else if (last == '-') {
		fnName.pop_back();
		return prefix + fnName + "_neg";
	}
	else if (last == '!') {
		fnName.pop_back();
		return prefix + fnName + "_not";
	}

	switch (dartFn.Kind()) {
	case DartFunction::CONSTRUCTOR: {
		std::string name = dartFn.IsStatic() ? "factory_ctor" : "ctor";
		ASSERT(fnName.starts_with(cls_prefix));
		if (fnName[cls_prefix.length()] == '.') {
			name += '_';
			name += &fnName[cls_prefix.length() + 1];
		}
		return name;
	}
	case DartFunction::SETTER:
		return "set_" + fnName;
	case DartFunction::GETTER:
		return "get_" + fnName;
	default:
		break;
	}

	return prefix + fnName;
}

void DartDumper::Dump4Ida(std::filesystem::path outDir)
{
	std::filesystem::create_directory(outDir);
	std::ofstream of((outDir / "addNames.py").string());
	of << "import ida_funcs\n";
	of << "import idaapi\n\n";
	std::ofstream ofNames((outDir / "semantic_names.txt").string());
	ofNames << "# semantic function rename (generated by blutter)\n";

	// nativeLib collects functions whose Code object owner is a Smi
	// (obfuscated apps). It is not part of app.libs, so process it explicitly.
	// Note: only its topClass holds these functions; other classes in
	// nativeLib are VM-internal classes without a library and should be skipped.
	const auto dumpLib4Ida = [&](DartLibrary* lib, bool onlyTopClass) {
		std::string lib_prefix = lib->GetName();
		std::vector<DartClass*> clses;
		if (onlyTopClass) {
			if (lib->topClass != nullptr)
				clses.push_back(lib->topClass);
		}
		else {
			clses = lib->classes;
		}
		for (auto cls : clses) {
			std::string cls_prefix = cls->Name();
			for (auto dartFn : cls->Functions()) {
				const auto ep = dartFn->Address();
				auto name = getFunctionName4Ida(*dartFn, cls_prefix);
				const auto fnSize = dartFn->Size();
				if (fnSize > 0) {
					of << std::format("ida_funcs.add_func({:#x}, {:#x})\n", ep, ep + fnSize);
				}
				// Semantic renaming: obfuscated functions registered during DumpCode
				// get a readable "fn_<token>" name; the legacy name is kept as a
				// comment and in semantic_names.txt. _miss/_check stay legacy-derived.
				std::string displayName = name;
				std::string semToken;
				if (auto it = fnSemanticClues_.find(ep); it != fnSemanticClues_.end()) {
					semToken = genSemanticFnToken(it->second.strings, it->second.calls);
					if (!semToken.empty())
						displayName = "fn_" + semToken;
				}
				of << std::format("idaapi.set_name({:#x}, \"{}_{}::{}_{:x}\")\n", ep, lib_prefix, cls_prefix, displayName.c_str(), ep);
				if (!semToken.empty()) {
					of << std::format("idaapi.set_cmt({:#x}, \"origin: {}\", 0)\n", ep, name.c_str());
					ofNames << std::format("{:#x} {} -> fn_{}\n", ep, name, semToken);
				}
				if (dartFn->HasMorphicCode()) {
					const auto payloadAddr = dartFn->PayloadAddress();
					const auto morphicAddr = dartFn->MonomorphicAddress();
					if (payloadAddr != 0 && payloadAddr != ep) {
						of << std::format("idaapi.set_name({:#x}, \"{}_{}::{}_{:x}_miss\")\n", payloadAddr, lib_prefix, cls_prefix, name.c_str(), ep);
					}
					if (morphicAddr != 0 && morphicAddr != ep && morphicAddr != payloadAddr) {
						of << std::format("idaapi.set_name({:#x}, \"{}_{}::{}_{:x}_check\")\n", morphicAddr, lib_prefix, cls_prefix, name.c_str(), ep);
					}
				}
			}
		}
	};

	for (auto lib : app.libs)
		dumpLib4Ida(lib, false);
	dumpLib4Ida(app.NativeLib(), true);

	for (auto& item : app.stubs) {
		auto stub = item.second;
		const auto ep = stub->Address();
		auto name = stub->FullName();
		std::replace(name.begin(), name.end(), '<', '@');
		std::replace(name.begin(), name.end(), '>', '@');
		std::replace(name.begin(), name.end(), ',', '&');
		std::replace(name.begin(), name.end(), ' ', '_');
		of << std::format("idaapi.set_name({:#x}, \"{}_{:x}\")\n", ep, name.c_str(), ep);
		if (stub->Size() == 0)
			continue;
		of << std::format("ida_funcs.add_func({:#x}, {:#x})\n", ep, ep + stub->Size());
	}


	// Note: create struct with a lot of member by ida script is very slow
	//   use header file then adding comment is much faster
	auto comments = DumpStructHeaderFile((outDir / "ida_dart_struct.h").string());
	of << R"CBLOCK(
import ida_funcs
import idaapi
import os
import idc

# ---- IDA version handling ----
# Note: 'import idc' succeeds on every IDA generation (idc still exists in 9.x),
# so it cannot detect the legacy API. The idc compat functions below
# (get_struc_id / import_type / op_stroff / set_member_cmt) work on both legacy
# IDA and IDA 9, so they are used unconditionally.
def _get_struc_id(name):
    return idc.get_struc_id(name)

def _import_type(til, name):
    return idc.import_type(til, name)

def _parse_types(file, flags):
    return idaapi.idc_parse_types(file, flags)

BADADDR = idaapi.BADADDR

def create_Dart_structs():
    sid1 = _get_struc_id("DartThread")
    sid2 = _get_struc_id("DartObjectPool")
    if sid1 == BADADDR:
        hdr_file = os.path.join(os.path.dirname(__file__), 'ida_dart_struct.h')
        _parse_types(hdr_file, idc.PT_FILE)
        sid1 = _import_type(-1, "DartThread")
        sid2 = _import_type(-1, "DartObjectPool")
    return sid1, sid2, sid2

def set_stroff(addr, op_idx, sid):
    idc.op_stroff(addr, op_idx, sid, 0)

def set_member_cmt(struct_or_tif, offset, cmt):
    idc.set_member_cmt(struct_or_tif, offset, cmt, True)
)CBLOCK";
	of << "sid1, sid2, pp_struct = create_Dart_structs()\n";

	of << "print('Applying Thread and Object Pool struct')\n";
	applyStruct4Ida(of);

	of << "print('Setting Object Pool comments')\n";
	for (const auto& [offset, comment] : comments) {
		of << "set_member_cmt(pp_struct, " << offset << ", '''" << comment << "''')\n";
	}

	of << "print('Script finished!')\n";
}

std::vector<std::pair<intptr_t, std::string>> DartDumper::DumpStructHeaderFile(std::string outFile)
{
	std::ofstream of(outFile);

	const auto max_offset = GetThreadMaxOffset();
	auto padNo = 0;
	of << "typedef struct DartThread {\n";
	for (intptr_t i = 0; i <= max_offset; i += 8) {
		auto& name = GetThreadOffsetName((int)i);
		if (name.empty()) {
			of << "\t__int64 pad" << std::hex << padNo << ";\n";
			padNo++;
		}
		else {
			of << "\t__int64 " << name << ";\n";
		}
	}
	of << "} DartThread;\n";

	of << "typedef struct DartObjectPool {\n";
	of << "\t__int64 pad0;\n";
	of << "\t__int64 pad1;\n";

	std::vector<std::pair<intptr_t, std::string>> comments;
	const auto& pool = app.GetObjectPool();
	intptr_t num = pool.Length();

	auto& obj = dart::Object::Handle();
	for (intptr_t i = num - 1; i >= 0; i--) {
		// the Dart Code access ObjectPool with offset that is not subtract by kHeapObjectTag (1)
		//   so we have to add 1 to make the offset same as offset in the code
		intptr_t offset = dart::ObjectPool::OffsetFromIndex(i) + 1;
		std::string name;

		auto objType = pool.TypeAt(i);
		if (objType == dart::ObjectPool::EntryType::kTaggedObject) {
			obj = pool.ObjectAt(i);
			if (obj.IsUnlinkedCall()) {
				// since Dart 3.10, target type might be kTaggedObject
				auto unlinkTargetType = pool.TypeAt(i + 1);
				if (unlinkTargetType == dart::ObjectPool::EntryType::kImmediate) {
					const auto imm = pool.RawValueAt(i + 1);
					auto dartFn = app.GetFunction(imm - app.base());
					if (dartFn != nullptr) {
						name = std::format("UnlinkedCall_{:#x}_{:#x}", offset, dartFn->Address(), offset);
					}
					else {
						name = std::format("UnlinkedCall_{:#x}_unknown", offset);
					}
				}
				else {
					ASSERT(unlinkTargetType == dart::ObjectPool::EntryType::kTaggedObject);
					const auto imm = pool.RawValueAt(i + 1);
					name = std::format("UnlinkedCall_{:#x}_tagged_{:#x}", offset, imm - app.base());
				}
			}
			else {
				// TODO: more meaningful variable name
				name = std::format("Obj_{:#x}", offset);
				auto comment = ObjectToString(obj);
				comments.push_back(std::make_pair(offset, comment));
			}
		}
		else if (objType == dart::ObjectPool::EntryType::kImmediate) {
			name = std::format("IMM_{:#x}_{:#x}", pool.RawValueAt(i), offset);
		}
		else if (objType == dart::ObjectPool::EntryType::kNativeFunction) {
			// the name of NativeFunction can be retrieved from dart::NativeSymbolResolver::LookupSymbolName
			//   but normally flutter code never access it
			// if we use the name, we should cache it because many Pool Objects reference same NativeFunction
			name = std::format("NativeFn_{:#x}_{:#x}", pool.RawValueAt(i), offset);
		}
		else {
			name = std::format("RAW_{:#x}_{:#x}", pool.RawValueAt(i), offset);
		}

		of << "\t__int64 " << name << ";\n";
	}

	of << "} DartObjectPool;\n";

	return comments;
}

void DartDumper::applyStruct4Ida(std::ostream& of)
{
	Disassembler disasmer;

	// nativeLib collects functions whose Code object owner is a Smi
	// (obfuscated apps). It is not part of app.libs, so process it explicitly.
	// Note: only its topClass holds these functions; other classes in
	// nativeLib are VM-internal classes without a library and should be skipped.
	const auto applyStruct4IdaLib = [&](DartLibrary* lib, bool onlyTopClass) {
		if (lib->isInternal)
			return;
		std::vector<DartClass*> clses;
		if (onlyTopClass) {
			if (lib->topClass != nullptr)
				clses.push_back(lib->topClass);
		}
		else {
			clses = lib->classes;
		}
		for (auto dartCls : clses) {
			for (auto dartFn : dartCls->Functions()) {
				if (dartFn->PayloadSize() == 0)
					continue;

				auto insns = disasmer.Disasm((uint8_t*)dartFn->PayloadAddress() + app.base(), dartFn->PayloadSize(), dartFn->Address());

				for (uint32_t i = 0; i < insns.Count(); i++) {
					auto insn = insns.At(i);
					const auto op_count = insn.op_count();

					for (uint8_t j = 0; j < op_count; j++) {
#if defined(TARGET_ARCH_ARM64)
						auto reg = ARM64_REG_INVALID;
						if (insn.ops[j].type == ARM64_OP_REG)
							reg = (arm64_reg)insn.ops[j].reg;
						else if (insn.ops[j].type == ARM64_OP_MEM)
							reg = (arm64_reg)insn.ops[j].mem.base;
						const bool isThr = (reg == CSREG_DART_THR);
						const bool isPp = (reg == CSREG_DART_PP);
#elif defined(TARGET_ARCH_X64)
						if (insn.ops[j].type != X86_OP_MEM)
							continue;
						const auto base = (x86_reg)insn.ops[j].mem.base;
						const bool isThr = IsCsDartThr(base);
						const bool isPp = IsCsDartPp(base);
#else
						const bool isThr = false;
						const bool isPp = false;
#endif
						if (isThr) {
							of << "set_stroff(" << insn.address() << ", " << (int)j << ", sid1)\n";
							break;
						}
						else if (isPp) {
							of << "set_stroff(" << insn.address() << ", " << (int)j << ", sid2)\n";
							break;
						}
					}
				}
			}
		}
	};

	for (auto lib : app.libs)
		applyStruct4IdaLib(lib, false);
	applyStruct4IdaLib(app.NativeLib(), true);
}

const std::string& DartDumper::getQuoteString(dart::Object& obj)
{
	const auto ptr = (intptr_t)obj.ptr();
	auto& txt = quoteStringCache[ptr];
	// because string is always quotes, empty string means inserting a new one
	if (txt.empty()) {
		txt = Util::UnescapeWithQuote(obj.ToCString());
	}
	return txt;
}

void DartDumper::DumpCode(const char* out_dir)
{
	std::filesystem::create_directory(out_dir);

	Disassembler disasmer;

	// nativeLib collects functions whose Code object owner is a Smi
	// (obfuscated apps). It is not part of app.libs, so process it explicitly.
	// Note: only its topClass holds these functions; other classes in
	// nativeLib are VM-internal classes without a library and should be skipped.
	const auto dumpLibCode = [&](DartLibrary* dartLib, bool onlyTopClass) {
		if (dartLib->isInternal)
			return;

		auto out_file = dartLib->CreatePath(out_dir);
		std::ofstream of(out_file);
		dartLib->PrintCommentInfo(of);

		std::vector<DartClass*> clses;
		if (onlyTopClass) {
			if (dartLib->topClass != nullptr)
				clses.push_back(dartLib->topClass);
		}
		else {
			clses = dartLib->classes;
		}
		for (auto dartCls : clses) {
			dartCls->PrintHead(of);

			if (!dartCls->Fields().empty())
				of << "\n";
			for (auto dartField : dartCls->Fields()) {
				dartField->Print(of);
			}

			if (!dartCls->Functions().empty())
				of << "\n";
			for (auto dartFn : dartCls->Functions()) {
				dartFn->PrintHead(of);

#ifndef NO_CODE_ANALYSIS
				// use as app is loaded at zero
				if (dartFn->Size() > 0) {
					auto& asmTexts = dartFn->GetAnalyzedData()->asmTexts.Data();
					{
						std::vector<std::string> clues;
						std::vector<std::string> typeClues;
						std::vector<std::string> callClues;
						std::set<std::string> seen;
						auto addClue = [&](std::string clue, bool toCrossRef, std::vector<std::string>* bucket = nullptr) {
							if (clue.empty() || !seen.insert(clue).second)
								return;
							(bucket ? *bucket : clues).push_back(clue);
							if (toCrossRef)
								stringToFuncs[clue].emplace_back(dartFn->Address(), dartFn->FullName());
						};
						// Semantic renaming: only obfuscated functions outside SDK
						// libraries are registered; Dump4Ida renames those by their
						// clues (see genSemanticFnToken / isObfuscatedFnName).
						const auto fnEp = dartFn->Address();
						const bool renameCandidate = !isSdkLib(dartLib) && isObfuscatedFnName(dartFn->Name());
						auto registerSem = [&](const std::string& clue, bool isCall) {
							if (!renameCandidate || clue.empty())
								return;
							auto& rec = fnSemanticClues_[fnEp];
							(isCall ? rec.calls : rec.strings).push_back(clue);
						};
						// pass 1: quoted strings (keep original clues first, they go into the cross-ref)
						for (auto& asmText : asmTexts) {
							if (asmText.dataType != AsmText::PoolOffset)
								continue;
							auto s = tryGetPoolString(asmText.poolOffset);
							if (s && isSemanticString(*s)) {
								addClue(*s, true);
								registerSem(*s, false);
							}
						}
						// pass 2: Field / Type / Function from the object pool
						for (auto& asmText : asmTexts) {
							if (asmText.dataType != AsmText::PoolOffset)
								continue;
							try {
								const auto& pool = app.GetObjectPool();
								intptr_t idx = dart::ObjectPool::IndexFromOffset(asmText.poolOffset);
								if (idx < 0 || idx >= pool.Length())
									continue;
								if (pool.TypeAt(idx) != dart::ObjectPool::EntryType::kTaggedObject)
									continue;
								auto& obj = dart::Object::Handle(pool.ObjectAt(idx));
								const auto cid = obj.GetClassId();
								if (cid == dart::kFieldCid) {
									addClue(fieldClueFromCString(dart::Field::Cast(obj).ToCString()), false);
								}
								else if (cid == dart::kTypeCid) {
									auto* t = app.typeDb->FindOrAdd(dart::Type::RawCast(obj.ptr()));
									if (t)
										addClue(typeClueFromName(t->ToString(false)), false, &typeClues);
								}
								else if (cid == dart::kFunctionCid) {
									auto fnBase = app.GetFunction(dart::Function::Cast(obj).entry_point() - app.base());
									auto callClue = callClueFromFn(fnBase);
									addClue(callClue, false, &callClues);
									registerSem(callClue, true);
								}
							}
							catch (...) {
							}
						}
						// pass 3: non-stub Call targets
						for (auto& asmText : asmTexts) {
							if (asmText.dataType != AsmText::Call)
								continue;
							auto* fn = app.GetFunction(asmText.callAddress);
							auto callClue = callClueFromFn(fn);
							addClue(callClue, false, &callClues);
							registerSem(callClue, true);
						}
						if (typeClues.size() > 2)
							typeClues.resize(2);
						if (callClues.size() > 4)
							callClues.resize(4);
						clues.insert(clues.end(), typeClues.begin(), typeClues.end());
						clues.insert(clues.end(), callClues.begin(), callClues.end());
						if (!clues.empty()) {
							of << "    // semantic: ";
							const size_t n = std::min(clues.size(), size_t{16});
							for (size_t i = 0; i < n; ++i) {
								if (i)
									of << ", ";
								of << clues[i];
							}
							if (clues.size() > 16)
								of << ", ...";
							of << "\n";
						}
					}
					auto& il_insns = dartFn->GetAnalyzedData()->il_insns;
					auto il_itr = il_insns.begin();
					const auto il_end = il_insns.end();
					AddrRange range;
					ASSERT(!asmTexts.empty());
					for (auto& asmText : asmTexts) {
						std::string extra;
						switch (asmText.dataType) {
						case AsmText::ThreadOffset:
							extra = "THR::" + GetThreadOffsetName(asmText.threadOffset);
							break;
						case AsmText::PoolOffset:
							extra = getPoolObjectDescription(asmText.poolOffset);
							break;
						case AsmText::Boolean:
							extra = asmText.boolVal ? "true" : "false";
							break;
						case AsmText::Call: {
							auto* fn = app.GetFunction(asmText.callAddress);
							if (fn) {
								extra = fn->FullName();
								auto retCid = fn->ReturnType();
								if (retCid != dart::kIllegalCid) {
									auto retCls = app.classes.at(retCid);
									extra += std::format(" -> {} (size={:#x})", retCls->FullName(), retCls->Size());
								}
							}
							break;
						}
						}

						of << "    // ";

						if (range.Has(asmText.addr)) {
							of << "    ";
						}
						else {
							while (il_itr != il_end && (*il_itr)->Start() < asmText.addr) {
								if ((*il_itr)->Kind() != ILInstr::Unknown) {
									of << std::format("{:#x}: {}\n", (*il_itr)->Start(), (*il_itr)->ToString());
									of << "    // ";
								}
								++il_itr;
							}
							if (il_itr != il_end && (*il_itr)->Start() == asmText.addr) {
								if ((*il_itr)->Kind() != ILInstr::Unknown) {
									of << std::format("{:#x}: {}\n", asmText.addr, (*il_itr)->ToString());
									of << "    //     ";
									range = (*il_itr)->Range();
								}
								++il_itr;
							}
						}

						if (extra.empty())
							of << std::format("{:#x}: {}\n", asmText.addr, &asmText.text[0]);
						else
							of << std::format("{:#x}: {}  ; {}\n", asmText.addr, &asmText.text[0], extra);
					}
				}
#endif // NO_CODE_ANALYSIS

				dartFn->PrintFoot(of);
			}

			dartCls->PrintFoot(of);
		}
	};

	for (auto dartLib : app.libs)
		dumpLibCode(dartLib, false);
	dumpLibCode(app.NativeLib(), true);
}

// collect instance ptr to dump the full contents in DumpObjects()
static std::set<intptr_t> knownObjectPtrs;

std::string DartDumper::ObjectToString(dart::Object& obj, bool simpleForm, bool nestedObj, int depth)
{
	const auto cid = obj.GetClassId();
	//auto dartCls = app_.classes[obj.GetClassId()];

	if (obj.IsString()) {
		auto& val = getQuoteString(obj);
		if (simpleForm || depth > 0)
			return val;
		return "String: " + val;
	}

	// use TypedData or TypedDataBase ?
	if (obj.IsTypedData()) {
		//dart::kTypedDataInt32ArrayCid;
		auto& arr = dart::TypedData::Cast(obj);
		const auto arr_len = arr.Length();
		auto ptr = arr.DataAddr(0);
		std::string txt;
		if (arr_len > 0) {
			switch (arr.ElementType()) {
#define ACCUMLATE(type) { \
	auto data = (type*)ptr; \
	txt = std::accumulate(data + 1, data + arr_len, std::format("[{:#x}", data[0]), [](std::string x, type y) { return x + ", " + std::format("{:#x}", y); } ); \
}
			case dart::kInt8ArrayElement:
				ACCUMLATE(int8_t);
				break;
			case dart::kUint8ArrayElement:
			case dart::kUint8ClampedArrayElement:
				ACCUMLATE(uint8_t);
				break;
			case dart::kInt16ArrayElement:
				ACCUMLATE(int16_t);
				break;
			case dart::kUint16ArrayElement:
				ACCUMLATE(uint16_t);
				break;
			case dart::kInt32ArrayElement:
				ACCUMLATE(int32_t);
				break;
			case dart::kUint32ArrayElement:
				ACCUMLATE(uint32_t);
				break;
			case dart::kInt64ArrayElement:
				ACCUMLATE(int64_t);
				break;
			case dart::kUint64ArrayElement:
				ACCUMLATE(uint64_t);
				break;
#undef ACCUMLATE
#define ACCUMLATE(type) { \
	auto data = (type*)ptr; \
	txt = std::accumulate(data + 1, data + arr_len, std::format("[{}", data[0]), [](std::string x, type y) { return x + ", " + std::format("{}", y); } ); \
}
			case dart::kFloat32ArrayElement:
				ACCUMLATE(float);
				break;
			case dart::kFloat64ArrayElement:
				ACCUMLATE(double);
				break;
#undef ACCUMLATE
			case dart::kFloat32x4ArrayElement:
			case dart::kInt32x4ArrayElement:
			case dart::kFloat64x2ArrayElement:
				FATAL("TODO: simd array");
			}

			txt += ']';
		}
		//arr.ElementSizeInBytes();
		return std::format("{}({}) {}", app.GetClass(cid)->Name(), arr_len, txt);
	}

	switch (cid) {
	case dart::kSmiCid:
		if (simpleForm || depth > 0)
			return std::format("{:#x}", dart::Smi::Cast(obj).Value());
		return std::format("Smi: {:#x}", dart::Smi::Cast(obj).Value());
	case dart::kMintCid:
		if (simpleForm || depth > 0)
			return std::format("{:#x}", MintValue(dart::Mint::Cast(obj)));
		return std::format("Mint: {:#x}", MintValue(dart::Mint::Cast(obj)));
	case dart::kDoubleCid:
		if (simpleForm || depth > 0)
			return std::format("{}", dart::Double::Cast(obj).value());
		return std::format("Double: {}", dart::Double::Cast(obj).value());
	case dart::kBoolCid:
		return dart::Bool::Cast(obj).value() ? "true" : "false";
	case dart::kNullCid:
		return "Null";
	case dart::kSentinelCid:
		return "Sentinel";
	case dart::kSubtypeTestCacheCid:
		return "SubtypeTestCache";
	case dart::kFunctionCid: {
		// stub never be in Object Pool
		auto fnBase = app.GetFunction(dart::Function::Cast(obj).entry_point() - app.base());
		if (fnBase == nullptr || fnBase->IsStub()) {
			return std::format("Function: [unknown] ({:#x})", dart::Function::Cast(obj).entry_point() - app.base());
		}
		auto dartFn = fnBase->AsFunction();
		if (dartFn->IsClosure()) {
			auto parentFn = dartFn->GetOutermostFunction();
			if (parentFn) {
				// AOT anonymous closure contains only static information
				return std::format("AnonymousClosure: {}({:#x}), in {} ({:#x})",
					dartFn->IsStatic() ? "static " : "", dartFn->Address(),
					parentFn->FullName(), parentFn->Address());
			}
			else {
				return std::format("AnonymousClosure: {}({:#x}), of {}",
					dartFn->IsStatic() ? "static " : "", dartFn->Address(),
					dartFn->Class().FullNameWithPackage());
			}
		}
		return std::format("Function: {} ({:#x})", dartFn->FullName(), dartFn->Address());
	}
	case dart::kClosureCid: {
		// TODO: show owner
		const auto& closure = dart::Closure::Cast(obj);
		if (!app.functions.contains(closure.entry_point() - app.base())) {
			std::cout << std::format("[!] missing closure at {:#x}\n", closure.entry_point() - app.base());
		}
		//RELEASE_ASSERT(app.functions.contains(closure.entry_point() - app.base()));
		return std::format("{} ({:#x})", closure.ToCString(), closure.entry_point());
	}
	case dart::kCodeCid: {
		const auto& code = dart::Code::Cast(obj);
		const auto ep = code.EntryPoint() - app.base();
		if (app.stubs.contains(ep)) {
			const auto stub = app.stubs[ep];
			return std::format("Stub: {} ({:#x})", stub->Name().c_str(), ep);
		}
		return std::format("Code: {} ({:#x})", code.ToCString(), ep);
	}
	case dart::kArrayCid:
	case dart::kImmutableArrayCid: {
		// Note: since Dart 3.7, Ojbect Pool is mutable. so, Array is used too
		// Objects in Object Pool immutable, so only immutable array is used for array
		// Most of no type arguments in Object Pool are Argument Descriptor
		const auto& arr = dart::Array::Cast(obj);
		const auto arr_len = arr.Length();
		const auto typeArg = app.typeDb->FindOrAdd(arr.GetTypeArguments());
		// without type arguments, assume it is argument descriptor. show it even simple form is true.
		if (simpleForm && typeArg->Length() > 0)
			return std::format("List{}({})", typeArg->ToString(), arr_len);

		std::ostringstream ss;
		if (arr_len > 0) {
			// in ImmutableList here, only Dart type (native type is not used)
			auto arrPtr = dart::Array::DataOf(arr.ptr());
			for (intptr_t i = 0; i < arr_len; i++) {
				if (i != 0)
					ss << ", ";

				if (arrPtr->IsHeapObject()) {
					obj = arrPtr->Decompress(app.heap_base());
					ss << ObjectToString(obj, simpleForm, nestedObj, depth + 1);
				}
				else {
					obj = arrPtr->DecompressSmi();
					ss << std::hex << std::showbase << dart::Smi::Cast(obj).Value();
				}
				arrPtr++;
			}
		}
		return std::format("List{}({}) [{}]", typeArg->ToString(), arr_len, ss.str());
	}
#ifdef HAS_RECORD_TYPE
	case dart::kRecordCid: {
		const auto& record = dart::Record::Cast(obj);
		std::ostringstream ss;
		const auto type = app.typeDb->FindOrAdd(DartGetRecordType(record));
		ss << "Record" << type->ToString() << " = (";
		auto& field = dart::Object::Handle();
		const auto num_fields = record.num_fields();
		for (intptr_t i = 0; i < num_fields; i++) {
			if (i != 0) ss << ", ";
			field = record.FieldAt(i);
			ss << ObjectToString(field, simpleForm, nestedObj, depth + 1);
		}
		ss << ")";
		return ss.str();
	}
#endif
	case dart::kTypeArgumentsCid:
		return "TypeArguments: " + app.typeDb->FindOrAdd(dart::TypeArguments::RawCast(obj.ptr()))->ToString();
	case dart::kTypeCid:
		return "Type: " + app.typeDb->FindOrAdd(dart::Type::RawCast(obj.ptr()))->ToString();
#ifdef HAS_RECORD_TYPE
	case dart::kRecordTypeCid:
		return "RecordType: " + app.typeDb->FindOrAdd(dart::RecordType::RawCast(obj.ptr()))->ToString();
#endif
	case dart::kTypeParameterCid:
		return "TypeParameter: " + app.typeDb->FindOrAdd(dart::TypeParameter::RawCast(obj.ptr()))->ToString();
	case dart::kFunctionTypeCid:
		return "FunctionType: " + app.typeDb->FindOrAdd(dart::FunctionType::RawCast(obj.ptr()))->ToString();
#ifdef HAS_TYPE_REF
	case dart::kTypeRefCid:
#endif
	case dart::kTypeParametersCid:
		// might be in a Type but not in Object Pool directly
		return std::format("{} (ptr: {:#x})", obj.ToCString(), (uint64_t)obj.ptr());
	case dart::kFieldCid: {
		const auto& field = dart::Field::Cast(obj);
		return std::format("{} (offset: {:#x})", field.ToCString(), field.TargetOffset());
	}
	case dart::kConstMapCid: {
		auto& map = dart::Map::Cast(obj);
		const auto typeArg = app.typeDb->FindOrAdd(map.GetTypeArguments());
		if (simpleForm)
			return std::format("Map{}({})", typeArg->ToString(), map.Length());

		std::ostringstream ss;
		std::string indent(depth * 2 + 2, ' ');
		ss << std::format("Map{}({}) {{\n", typeArg->ToString(), map.Length());
		dart::Map::Iterator iter(map);
		auto& key = dart::Object::Handle();
		auto& val = dart::Object::Handle();
		int cnt = 0;
		while (iter.MoveNext()) {
			if (cnt++) ss << ",\n";
			key = iter.CurrentKey();
			val = iter.CurrentValue();
			// key always be simple form
			ss << indent << ObjectToString(key, true, false, depth + 1) << ": " << ObjectToString(val, simpleForm, nestedObj, depth + 1);
		}
		if (cnt) ss << "\n";
		ss << std::string(depth * 2, ' ') << "}";
		return ss.str();
	}
	case dart::kConstSetCid: {
		auto& set = dart::Set::Cast(obj);
		const auto typeArg = app.typeDb->FindOrAdd(set.GetTypeArguments());
		if (simpleForm)
			return std::format("Set{}({})", typeArg->ToString(), set.Length());

		std::ostringstream ss;
		ss << std::format("Set{}({}) {{ ", typeArg->ToString(), set.Length());
		dart::Set::Iterator iter(set);
		auto& key = dart::Object::Handle();
		int cnt = 0;
		while (iter.MoveNext()) {
			if (cnt++)
				ss << ", ";
			key = iter.CurrentKey();
			ss << ObjectToString(key, simpleForm, nestedObj, depth + 1);
		}
		ss << " }";
		return ss.str();
	}
	case dart::kLibraryPrefixCid: {
		const auto& libPrefix = dart::LibraryPrefix::Cast(obj);
		const auto& name = dart::String::Handle(libPrefix.name());
		RELEASE_ASSERT(libPrefix.num_imports() == 1);
		// don't know what importer is
		//const auto& importer = dart::Library::Handle(libPrefix.importer());
		const auto& imports = dart::Array::Handle(libPrefix.imports());
		const auto& importObj = dart::Object::Handle(imports.At(0));
		RELEASE_ASSERT(importObj.GetClassId() == dart::kNamespaceCid);
		const auto& ns = dart::Namespace::Cast(importObj);
		const auto& lib = dart::Library::Handle(ns.target());
		const auto& libName = dart::String::Handle(lib.url());
		return std::format("LibraryPrefix: {}, target lib: {} ({})", name.ToCString(), libName.ToCString(), lib.toplevel_class().untag()->id());
	}
	case dart::kInt32x4Cid: {
		const auto& simd = dart::Int32x4::Cast(obj);
		return std::format("Int32x4: ({}, {}, {}, {})", simd.x(), simd.y(), simd.z(), simd.w());
	}
	case dart::kFloat32x4Cid: {
		const auto& simd = dart::Float32x4::Cast(obj);
		return std::format("Float32x4: ({}, {}, {}, {})", simd.x(), simd.y(), simd.z(), simd.w());
	}
	case dart::kFloat64x2Cid: {
		const auto& simd = dart::Float64x2::Cast(obj);
		return std::format("Float64x2: ({}, {})", simd.x(), simd.y());
	}
	case dart::kInstanceCid:
		return std::format("Obj!Object@{:x}", (uint32_t)(intptr_t)obj.ptr());
	// TODO: enum subclass
	}

	// many cids are instance. handling them after special classes.
	ASSERT(obj.IsInstance());

	if (cid < dart::kNumPredefinedCids) {
		FATAL("Unhandle internal class %s (%ld)", app.GetClass(cid)->Name().c_str(), cid);
	}

	// TODO: print library and package prefix
	knownObjectPtrs.insert((intptr_t)obj.ptr());
	return dumpInstance(obj, simpleForm, nestedObj, depth);
}

std::string DartDumper::dumpInstance(dart::Object& obj, bool simpleForm, bool nestedObj, int depth)
{
	auto dartCls = app.classes[obj.GetClassId()];
	ASSERT(dartCls->Id() >= dart::kNumPredefinedCids);

	std::string closeIndent(depth * 2, ' ');
	std::string indent(closeIndent.length() + 2, ' ');

	const auto ptr = dart::UntaggedObject::ToAddr(obj.ptr());
	DartType* dtype = app.typeDb->FindOrAdd(*dartCls, dart::Instance::Cast(obj));
	if (simpleForm || (!nestedObj && depth > 0)) {
		return std::format("Obj!{}@{:x}", dtype->ToString(), (uint32_t)(intptr_t)obj.ptr());
	}

	std::vector<DartClass*> parents;
	auto superCls = dartCls->Parent();
	while (superCls != nullptr && superCls->Id() != dart::kInstanceCid) {
		parents.push_back(superCls);
		superCls = superCls->Parent();
	}

	std::ostringstream ss;
	int fieldCnt = 0;
	ss << std::format("Obj!{}@{:x} : {{\n", dtype->ToString(), (uint32_t)(intptr_t)obj.ptr());
	auto offset = dart::Instance::NextFieldOffset();
	for (auto parent : parents | std::views::reverse) {
		if (offset < parent->Size()) {
			// parent fields depth MUST increment by 2 because 1 is for "Super!..."
			auto txt = dumpInstanceFields(obj, *parent, ptr, offset, simpleForm, nestedObj, depth + 2);
			offset = parent->Size();
			if (!txt.empty()) {
				if (fieldCnt++)
					ss << ",\n";
				ss << indent << "Super!" << parent->FullName() << " : {\n";
				ss << txt << "\n";
				ss << indent << "}";
			}
		}
	}

	auto fieldTxt = dumpInstanceFields(obj, *dartCls, ptr, offset, simpleForm, nestedObj, depth + 1);
	if (!fieldTxt.empty()) {
		if (fieldCnt++)
			ss << ",\n";
		ss << fieldTxt;
	}
	if (fieldCnt)
		ss << "\n";
	ss << closeIndent << "}";

	return ss.str();
}

std::string DartDumper::dumpInstanceFields(dart::Object& obj, DartClass& dartCls, intptr_t ptr, intptr_t offset, bool simpleForm, bool nestedObj, int depth)
{
	std::stringstream ss;
	std::string indent(depth * 2, ' ');

	const auto bitmap = dartCls.UnboxedFieldsBitmap();
	while (offset < dartCls.Size()) {
		std::string txtField;
		// TODO: match the offset to field name if possible
		if (bitmap.Get(offset / dart::kCompressedWordSize)) {
			// AOT uses native integer if it is less than 31 bits (compressed pointer)
			// integer (4/8 bytes) or double (8 bytes)
			if (dart::kCompressedWordSize == 4)
				RELEASE_ASSERT(bitmap.Get((offset + dart::kCompressedWordSize) / dart::kCompressedWordSize));
			auto p = reinterpret_cast<uint64_t*>(ptr + offset);
			// it is rare to find integer that larger than 0x1000_0000_0000_0000
			if (*p <= 0x1000000000000000 || *p >= 0xffffffffffff0000) {
				txtField = std::format("off_{:x}: int({:#x})", offset, *p);
			}
			else {
				txtField = std::format("off_{:x}: double({})", offset, *((double*)p));
			}
			offset += dart::kCompressedWordSize;
		}
		else if (offset != dartCls.TypeArgumentsOffset()) {
			// compressed object ptr
			auto p = reinterpret_cast<dart::CompressedObjectPtr*>(ptr + offset);
			if (*p != dart::CompressedObjectPtr(nullptr)) {
				if (p->IsHeapObject()) {
					auto objPtr2 = p->Decompress(app.heap_base());
					if (objPtr2 != nullptr && objPtr2.GetClassId() != dart::kNullCid) {
						obj = objPtr2;
						if (simpleForm || objPtr2.GetClassId() < dart::kNumPredefinedCids)
							txtField = std::format("off_{:x}: {}", offset, ObjectToString(obj, simpleForm, nestedObj, depth));
						else
							txtField = std::format("off_{:x}_{}", offset, ObjectToString(obj, simpleForm, nestedObj, depth));
					}
				}
				else {
					obj = p->DecompressSmi();
					txtField = std::format("off_{:x}_Smi: {:#x}", offset, dart::Smi::Cast(obj).Value());
				}
			}
		}
		offset += dart::kCompressedWordSize;

		if (!txtField.empty()) {
			if (ss.tellp() != 0)
				ss << ",\n";
			ss << indent << txtField;
		}
	}

	return ss.str();
}

std::string DartDumper::getPoolObjectDescription(intptr_t offset, bool simpleForm)
{
	const auto& pool = app.GetObjectPool();
	intptr_t idx = dart::ObjectPool::IndexFromOffset(offset);
	auto objType = pool.TypeAt(idx);
	// see how the EntryType is handled from vm/object_service.cc - ObjectPool::PrintJSONImpl()
	if (objType == dart::ObjectPool::EntryType::kTaggedObject) {
		auto& obj = dart::Object::Handle(pool.ObjectAt(idx));
		if (obj.IsUnlinkedCall()) {
			// since Dart 3.10, target type might be kTaggedObject
			auto unlinkTargetType = pool.TypeAt(idx + 1);
			if (unlinkTargetType == dart::ObjectPool::EntryType::kImmediate) {
				const auto imm = pool.RawValueAt(idx + 1);
				auto dartFn = app.GetFunction(imm - app.base());
				if (dartFn != nullptr) {
					return std::format("[pp+{:#x}] UnlinkedCall: {:#x} - {}", offset, dartFn->Address(), dartFn->FullName().c_str());
				}
				else {
					return std::format("[pp+{:#x}] UnlinkedCall: {:#x} - [unknown function]", offset, imm - app.base());
				}
			}
			else {
				ASSERT(unlinkTargetType == dart::ObjectPool::EntryType::kTaggedObject);
				auto& obj2 = dart::Object::Handle(pool.ObjectAt(idx + 1));
				return std::format("[pp+{:#x}] UnlinkedCall: {}", offset, ObjectToString(obj2, simpleForm));
			}
		}
		return std::format("[pp+{:#x}] {}", offset, ObjectToString(obj, simpleForm));
	}
	else if (objType == dart::ObjectPool::EntryType::kImmediate) {
		dart::uword imm = pool.RawValueAt(idx);
		if (imm <= 0x1000000000000000 || imm >= 0xffffffffffff0000) {
			return std::format("[pp+{:#x}] IMM: {:#x}", offset, imm);
		}
		else {
			return std::format("[pp+{:#x}] IMM: double({}) from {:#x}", offset, *((double*)&imm), imm);
		}
	}
	else if (objType == dart::ObjectPool::EntryType::kNativeFunction) {
		auto pc = pool.RawValueAt(idx);
		uintptr_t start = 0;
		auto name = dart::NativeSymbolResolver::LookupSymbolName(pc, &start);
		if (name != NULL) {
			auto txt = std::format("[pp+{:#x}] NativeFn: {} at {:#x}", offset, name, pc);
			dart::NativeSymbolResolver::FreeSymbolName(name);
			return txt;
		}
		else {
			return std::format("[pp+{:#x}] NativeFn: [no name] at {:#x}", offset, pc);
		}
	}
	else {
		throw std::runtime_error(std::format("unknown pool object type: {}", (int)objType).c_str());
	}
}

std::optional<std::string> DartDumper::tryGetPoolString(intptr_t offset)
{
	try {
		const auto& pool = app.GetObjectPool();
		intptr_t idx = dart::ObjectPool::IndexFromOffset(offset);
		if (idx < 0 || idx >= pool.Length())
			return std::nullopt;
		if (pool.TypeAt(idx) != dart::ObjectPool::EntryType::kTaggedObject)
			return std::nullopt;
		auto& obj = dart::Object::Handle(pool.ObjectAt(idx));
		if (!obj.IsString())
			return std::nullopt;
		return getQuoteString(obj);
	}
	catch (...) {
		return std::nullopt;
	}
}

void DartDumper::DumpStringCrossRef(const char* filename)
{
	std::ofstream of(filename);
	of << "# string -> functions that reference it (from object pool)\n";
	of << "# generated by blutter semantic dump\n\n";
	size_t nstr = 0, nref = 0;
	for (const auto& [s, fns] : stringToFuncs) {
		nstr++;
		nref += fns.size();
		of << s << "\n";
		for (const auto& [addr, name] : fns)
			of << std::format("    {:#x}  {}\n", addr, name);
		of << "\n";
	}
	std::cout << std::format("[+] string cross-ref: {} strings, {} function refs\n", nstr, nref);
}

void DartDumper::DumpObjectPool(const char* filename)
{
	std::ofstream of(filename);
	const auto& pool = app.GetObjectPool();
	intptr_t num = pool.Length();

	const auto& rawObj = pool.ptr()->untag();
	const auto raw_addr = dart::UntaggedObject::ToAddr(rawObj);
	of << std::format("pool heap offset: {:#x}\n", raw_addr - app.heap_base());

	for (intptr_t i = 0; i < num; i++) {
		// offset here is from ObjectPool pointer subtracted by kHeapObjectTag
		// add 1 to make the offset value same as offset in compiled code
		intptr_t offset = dart::ObjectPool::OffsetFromIndex(i);
		auto txt = getPoolObjectDescription(offset + 1, false);
		of << txt << "\n";
		if (txt.compare(txt.find(']'), 15, "] UnlinkedCall:") == 0)
			i++;
	}
}

void DartDumper::DumpObjects(const char* filename)
{
	std::ofstream of(filename);

	auto& obj = dart::Object::Handle();
	for (auto objPtr : knownObjectPtrs) {
		obj = dart::ObjectPtr(objPtr);
		const bool simpleForm = false;
		const bool nestedObj = true;
		of << dumpInstance(obj, simpleForm, nestedObj, 0);
		of << "\n\n";
	}
}
