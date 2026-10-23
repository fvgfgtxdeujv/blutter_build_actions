#include "pch.h"
#include "JsonDumper.h"
#include "DartApp.h"
#include "DartClass.h"
#include "DartField.h"
#include "DartFunction.h"
#include "DartLibrary.h"
#include "DartTypes.h"
#include <filesystem>
#include <fstream>
#include <ostream>

namespace {

// Minimal JSON string escaping. UTF-8 bytes are passed through untouched; only
// quotes, backslashes and control characters need escaping.
std::string JsonQuote(const std::string& s)
{
	std::string out;
	out.reserve(s.size() + 2);
	out += '"';
	for (unsigned char c : s) {
		switch (c) {
		case '"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if (c < 0x20) {
				char buf[8];
				snprintf(buf, sizeof(buf), "\\u%04x", c);
				out += buf;
			}
			else {
				out += static_cast<char>(c);
			}
			break;
		}
	}
	out += '"';
	return out;
}

const char* KindName(DartFunction::FunctionKind kind)
{
	switch (kind) {
	case DartFunction::CONSTRUCTOR: return "constructor";
	case DartFunction::GETTER: return "getter";
	case DartFunction::SETTER: return "setter";
	default: return "normal";
	}
}

const char* ClassTypeName(DartClass::ClassType type)
{
	switch (type) {
	case DartClass::ABSTRACT: return "abstract";
	case DartClass::ENUM: return "enum";
	default: return "class";
	}
}

} // namespace

namespace {

// Library URLs contain ':' and '/', which are unusable in file names.
std::string Slug(const std::string& name)
{
	std::string out;
	out.reserve(name.size());
	for (char c : name) {
		const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
			(c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
		out += ok ? c : '_';
	}
	if (out.size() > 48)
		out.resize(48);
	return out.empty() ? "lib" : out;
}

constexpr int kSchemaVersion = 1;

} // namespace

void JsonDumper::Write(const char* filename, bool split)
{
	const auto emitFields = [](std::ostream& out, DartClass* dartCls) {
		out << "\"fields\":[";
		bool first = true;
		for (auto* field : dartCls->Fields()) {
			if (!first)
				out << ',';
			first = false;
			out << "{\"name\":" << JsonQuote(field->Name());
			out << ",\"offset\":" << field->Offset();
			out << ",\"type\":" << (field->Type() ? JsonQuote(field->Type()->ToString()) : "null");
			out << ",\"static\":" << (field->IsStatic() ? "true" : "false");
			out << ",\"final\":" << (field->IsFinal() ? "true" : "false");
			out << ",\"const\":" << (field->IsConst() ? "true" : "false");
			out << ",\"late\":" << (field->IsLate() ? "true" : "false");
			out << "}";
		}
		out << "]";
	};

	const auto emitFunctions = [](std::ostream& out, DartClass* dartCls) {
		out << "\"functions\":[";
		bool first = true;
		for (auto* fn : dartCls->Functions()) {
			if (!first)
				out << ',';
			first = false;
			auto& sig = fn->Signature();
			out << "{\"address\":" << fn->Address();
			out << ",\"name\":" << JsonQuote(fn->Name());
			out << ",\"fullName\":" << JsonQuote(fn->FullName());
			out << ",\"size\":" << fn->Size();
			out << ",\"kind\":\"" << KindName(fn->Kind()) << "\"";
			out << ",\"static\":" << (fn->IsStatic() ? "true" : "false");
			out << ",\"const\":" << (fn->IsConst() ? "true" : "false");
			out << ",\"abstract\":" << (fn->IsAbstract() ? "true" : "false");
			out << ",\"closure\":" << (fn->IsClosure() ? "true" : "false");
			out << ",\"generic\":" << (fn->IsGeneric() ? "true" : "false");
			// Untagged closure context offset; -1 when the function is not a closure
			// or captures nothing. Per-function value since Dart 3.13's elements[].
			out << ",\"closureContextOffset\":" << fn->ClosureContextOffset();
			out << ",\"returnType\":" << (sig.ReturnType() ? JsonQuote(sig.ReturnType()->ToString()) : "null");

			out << ",\"typeParameters\":[";
			bool firstTp = true;
			for (auto& tp : sig.TypeParameterNames()) {
				if (!firstTp)
					out << ',';
				firstTp = false;
				out << JsonQuote(tp);
			}
			out << "]";

			out << ",\"params\":[";
			bool firstParam = true;
			for (auto& param : sig.Params()) {
				if (!firstParam)
					out << ',';
				firstParam = false;
				out << "{\"name\":" << JsonQuote(param.name);
				out << ",\"type\":" << (param.type ? JsonQuote(param.type->ToString()) : "null");
				out << ",\"required\":" << (param.isRequired ? "true" : "false");
				out << "}";
			}
			out << "]}";
		}
		out << "]";
	};

	const auto emitClass = [&](std::ostream& out, DartClass* dartCls) {
		out << "{\"id\":" << dartCls->Id();
		out << ",\"name\":" << JsonQuote(dartCls->Name());
		out << ",\"fullName\":" << JsonQuote(dartCls->FullName());
		out << ",\"library\":" << JsonQuote(dartCls->Library().url);
		out << ",\"parent\":" << (dartCls->Parent() ? static_cast<int64_t>(dartCls->Parent()->Id()) : -1);
		out << ",\"size\":" << dartCls->Size();
		out << ",\"typeArgumentOffset\":" << dartCls->TypeArgumentOffset();
		out << ",\"type\":\"" << ClassTypeName(dartCls->Type()) << "\"";
		out << ',';
		emitFields(out, dartCls);
		out << ',';
		emitFunctions(out, dartCls);
		out << "}";
	};

	// nativeLib only keeps its top class; other classes there are VM internals
	// without a library (mirrors CodeAnalyzer::AnalyzeAll).
	const auto classesOf = [](DartLibrary* lib) {
		std::vector<DartClass*> classes;
		if (lib->isInternal) {
			if (lib->topClass)
				classes.push_back(lib->topClass);
		}
		else {
			for (auto* dartCls : lib->classes) {
				if (dartCls)
					classes.push_back(dartCls);
			}
		}
		return classes;
	};

	const auto libraryName = [](DartLibrary* lib) {
		return lib->url.empty() ? std::string("<native>") : lib->url;
	};

	const auto emitLibrary = [&](std::ostream& out, DartLibrary* lib) {
		out << "{\"name\":" << JsonQuote(libraryName(lib));
		out << ",\"url\":" << JsonQuote(lib->url);
		out << ",\"internal\":" << (lib->isInternal ? "true" : "false");
		out << ",\"classes\":[";
		bool first = true;
		for (auto* dartCls : classesOf(lib)) {
			if (!first)
				out << ',';
			first = false;
			emitClass(out, dartCls);
		}
		out << "]}";
	};

	std::vector<DartLibrary*> libs(app.libs.begin(), app.libs.end());
	libs.push_back(app.NativeLib());

	if (!split) {
		std::ofstream of(filename, std::ios_base::binary);
		if (!of) {
			std::cerr << "Failed to open " << filename << "\n";
			return;
		}
		// No absolute addresses: every function address is image-relative so the
		// file is byte-identical for the same input regardless of ASLR.
		of << "{\n\"schemaVersion\":" << kSchemaVersion << ",\n\"libraries\":[\n";
		bool first = true;
		for (auto* lib : libs) {
			if (!first)
				of << ",\n";
			first = false;
			emitLibrary(of, lib);
		}
		of << "\n]\n}\n";
		return;
	}

	// Split mode: the main file lists every library with its per-library file, so
	// a consumer can diff one library without reading a multi-megabyte document.
	std::filesystem::path mainPath(filename);
	std::filesystem::path libDir = mainPath;
	libDir += ".d";
	std::error_code ec;
	std::filesystem::create_directories(libDir, ec);
	if (ec) {
		std::cerr << "Failed to create " << libDir.string() << ": " << ec.message() << "\n";
		return;
	}

	std::ofstream of(filename, std::ios_base::binary);
	if (!of) {
		std::cerr << "Failed to open " << filename << "\n";
		return;
	}
	of << "{\n\"schemaVersion\":" << kSchemaVersion << ",\n\"split\":true,\n\"libraries\":[\n";

	struct Entry {
		std::string file;
		DartLibrary* lib;
		size_t classes;
		size_t functions;
	};
	std::vector<Entry> entries;
	entries.reserve(libs.size());
	for (auto* lib : libs) {
		const auto classes = classesOf(lib);
		size_t functions = 0;
		for (auto* dartCls : classes)
			functions += dartCls->Functions().size();
		entries.push_back(Entry{
			std::format("{:04d}-{}.json", entries.size(), Slug(libraryName(lib))),
			lib, classes.size(), functions });
	}

	bool first = true;
	for (auto& entry : entries) {
		if (!first)
			of << ",\n";
		first = false;
		of << "{\"name\":" << JsonQuote(libraryName(entry.lib));
		of << ",\"url\":" << JsonQuote(entry.lib->url);
		of << ",\"internal\":" << (entry.lib->isInternal ? "true" : "false");
		of << ",\"file\":" << JsonQuote(entry.file);
		of << ",\"classes\":" << entry.classes;
		of << ",\"functions\":" << entry.functions;
		of << "}";
	}
	of << "\n]\n}\n";
	of.close();

	for (auto& entry : entries) {
		std::ofstream lf(libDir / entry.file, std::ios_base::binary);
		if (!lf) {
			std::cerr << "Failed to open " << (libDir / entry.file).string() << "\n";
			continue;
		}
		lf << "{\n\"schemaVersion\":" << kSchemaVersion << ",\n\"library\":";
		emitLibrary(lf, entry.lib);
		lf << "\n}\n";
	}
}
