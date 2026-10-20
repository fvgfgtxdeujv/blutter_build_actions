#include "pch.h"
#include "JsonDumper.h"
#include "DartApp.h"
#include "DartClass.h"
#include "DartField.h"
#include "DartFunction.h"
#include "DartLibrary.h"
#include "DartTypes.h"
#include <fstream>

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

void JsonDumper::Write(const char* filename)
{
	std::ofstream of(filename, std::ios_base::binary);
	if (!of) {
		std::cerr << "Failed to open " << filename << "\n";
		return;
	}

	const auto writeFields = [&](DartClass* dartCls) {
		of << "\"fields\":[";
		bool first = true;
		for (auto* field : dartCls->Fields()) {
			if (!first)
				of << ',';
			first = false;
			of << "{\"name\":" << JsonQuote(field->Name());
			of << ",\"offset\":" << field->Offset();
			of << ",\"type\":" << (field->Type() ? JsonQuote(field->Type()->ToString()) : "null");
			of << ",\"static\":" << (field->IsStatic() ? "true" : "false");
			of << ",\"final\":" << (field->IsFinal() ? "true" : "false");
			of << ",\"const\":" << (field->IsConst() ? "true" : "false");
			of << ",\"late\":" << (field->IsLate() ? "true" : "false");
			of << "}";
		}
		of << "]";
	};

	const auto writeFunctions = [&](DartClass* dartCls) {
		of << "\"functions\":[";
		bool first = true;
		for (auto* fn : dartCls->Functions()) {
			if (!first)
				of << ',';
			first = false;
			auto& sig = fn->Signature();
			of << "{\"address\":" << fn->Address();
			of << ",\"name\":" << JsonQuote(fn->Name());
			of << ",\"fullName\":" << JsonQuote(fn->FullName());
			of << ",\"size\":" << fn->Size();
			of << ",\"kind\":\"" << KindName(fn->Kind()) << "\"";
			of << ",\"static\":" << (fn->IsStatic() ? "true" : "false");
			of << ",\"const\":" << (fn->IsConst() ? "true" : "false");
			of << ",\"abstract\":" << (fn->IsAbstract() ? "true" : "false");
			of << ",\"closure\":" << (fn->IsClosure() ? "true" : "false");
			of << ",\"generic\":" << (fn->IsGeneric() ? "true" : "false");
			// Untagged closure context offset; -1 when the function is not a closure
			// or captures nothing. Per-function value since Dart 3.13's elements[].
			of << ",\"closureContextOffset\":" << fn->ClosureContextOffset();
			of << ",\"returnType\":" << (sig.ReturnType() ? JsonQuote(sig.ReturnType()->ToString()) : "null");

			of << ",\"typeParameters\":[";
			bool firstTp = true;
			for (auto& tp : sig.TypeParameterNames()) {
				if (!firstTp)
					of << ',';
				firstTp = false;
				of << JsonQuote(tp);
			}
			of << "]";

			of << ",\"params\":[";
			bool firstParam = true;
			for (auto& param : sig.Params()) {
				if (!firstParam)
					of << ',';
				firstParam = false;
				of << "{\"name\":" << JsonQuote(param.name);
				of << ",\"type\":" << (param.type ? JsonQuote(param.type->ToString()) : "null");
				of << ",\"required\":" << (param.isRequired ? "true" : "false");
				of << "}";
			}
			of << "]}";
		}
		of << "]";
	};

	const auto writeClass = [&](DartClass* dartCls) {
		of << "{\"id\":" << dartCls->Id();
		of << ",\"name\":" << JsonQuote(dartCls->Name());
		of << ",\"fullName\":" << JsonQuote(dartCls->FullName());
		of << ",\"library\":" << JsonQuote(dartCls->Library().url);
		of << ",\"parent\":" << (dartCls->Parent() ? static_cast<int64_t>(dartCls->Parent()->Id()) : -1);
		of << ",\"size\":" << dartCls->Size();
		of << ",\"typeArgumentOffset\":" << dartCls->TypeArgumentOffset();
		of << ",\"type\":\"" << ClassTypeName(dartCls->Type()) << "\"";
		of << ',';
		writeFields(dartCls);
		of << ',';
		writeFunctions(dartCls);
		of << "}";
	};

	const auto writeLibrary = [&](DartLibrary* lib) {
		const std::string name = lib->url.empty() ? std::string("<native>") : lib->url;
		of << "{\"name\":" << JsonQuote(name);
		of << ",\"url\":" << JsonQuote(lib->url);
		of << ",\"internal\":" << (lib->isInternal ? "true" : "false");
		of << ",\"classes\":[";
		bool first = true;
		const auto emitClass = [&](DartClass* dartCls) {
			if (!dartCls)
				return;
			if (!first)
				of << ',';
			first = false;
			writeClass(dartCls);
		};
		// nativeLib only keeps its top class; other classes there are VM internals
		// without a library (mirrors CodeAnalyzer::AnalyzeAll).
		if (lib->isInternal) {
			emitClass(lib->topClass);
		}
		else {
			for (auto* dartCls : lib->classes)
				emitClass(dartCls);
		}
		of << "]}";
	};

	// No absolute addresses: every function address is image-relative so the
	// file is byte-identical for the same input regardless of ASLR.
	of << "{\n";
	of << "\"libraries\":[\n";
	bool firstLib = true;
	for (auto* lib : app.libs) {
		if (!firstLib)
			of << ",\n";
		firstLib = false;
		writeLibrary(lib);
	}
	if (!firstLib)
		of << ",\n";
	writeLibrary(app.NativeLib());
	of << "\n]\n}\n";
}
