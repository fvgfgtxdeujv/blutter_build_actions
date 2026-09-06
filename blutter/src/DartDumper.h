#pragma once
#include "DartApp.h"
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

// Point the semantic-clue blacklist loader at an external file
// (see --blacklist in main.cpp and the loader in DartDumper.cpp).
void SetSemanticBlacklistFile(const std::string& path);

// Semantic clues of one function, collected during DumpCode and consumed by
// Dump4Ida to give obfuscated functions a readable name (see isObfuscatedFnName).
struct FnSemanticClues {
	std::vector<std::string> strings; // quoted dart strings, reference order, isSemanticString-filtered
	std::vector<std::string> calls;   // call:Class::method clues, reference order
};

class DartDumper
{
public:
	DartDumper(DartApp& app) : app(app) {};

	void Dump4Ida(std::filesystem::path outDir);

	std::vector<std::pair<intptr_t, std::string>> DumpStructHeaderFile(std::string outFile);

	void DumpCode(const char* out_dir);

	void DumpObjectPool(const char* filename);
	void DumpObjects(const char* filename);
	void DumpStringCrossRef(const char* filename);

	std::string ObjectToString(dart::Object& obj, bool simpleForm = false, bool nestedObj = false, int depth = 0);

private:
	std::string getPoolObjectDescription(intptr_t offset, bool simpleForm = true);
	std::optional<std::string> tryGetPoolString(intptr_t offset);

	std::string dumpInstance(dart::Object& obj, bool simpleForm = false, bool nestedObj = false, int depth = 0);
	std::string dumpInstanceFields(dart::Object& obj, DartClass& dartCls, intptr_t ptr, intptr_t offset, bool simpleForm = false, bool nestedObj = false, int depth = 0);

	void applyStruct4Ida(std::ostream& of);

	const std::string& getQuoteString(dart::Object& obj);

	DartApp& app;
	// map for object ptr to unescape string with quote
	std::unordered_map<intptr_t, std::string> quoteStringCache;
	// quoted dart string -> (fn address, FullName) collected during DumpCode
	std::map<std::string, std::vector<std::pair<uint64_t, std::string>>> stringToFuncs;
	// fn entry address -> semantic clues, registered for obfuscated functions only.
	// Empty when code analysis is disabled (NO_CODE_ANALYSIS); Dump4Ida then keeps legacy names.
	std::unordered_map<uint64_t, FnSemanticClues> fnSemanticClues_;
};
