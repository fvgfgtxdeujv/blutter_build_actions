#include "pch.h"
#include "CodeAnalyzer.h"
#include "DartApp.h"

#include <atomic>
#include <optional>
#include <thread>
#include <chrono>

#ifndef NO_CODE_ANALYSIS

AnalyzedFnData::AnalyzedFnData(DartApp& app, DartFunction& dartFn, AsmTexts asmTexts)
	: app(app), dartFn(dartFn), asmTexts(std::move(asmTexts))
{
}

void CodeAnalyzer::AnalyzeAll(bool strict, int jobs)
{
	size_t analyzed = 0;
	size_t failed = 0;
	std::vector<std::string> failures;

	// Collect first so a chunk can be disassembled by several threads while the
	// calling thread keeps converting to IL in the original order.
	std::vector<DartFunction*> fns;
	const auto collectLib = [&](DartLibrary* lib, bool onlyTopClass) {
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
		for (auto cls : clses) {
			for (auto dartFn : cls->Functions()) {
				if (dartFn->Size() == 0)
					continue;
				fns.push_back(dartFn);
			}
		}
	};
	// nativeLib collects functions whose Code object owner is a Smi
	// (obfuscated apps). It is not part of app.libs, so process it explicitly.
	// Note: only its topClass holds these functions; other classes in
	// nativeLib are VM-internal classes without a library and should be skipped.
	for (auto lib : app.libs)
		collectLib(lib, false);
	collectLib(app.NativeLib(), true);

	const auto convertOne = [&](DartFunction* dartFn) {
		auto asm_insns = Disassembler{}.Disasm((uint8_t*)dartFn->MemAddress(), dartFn->Size(), dartFn->Address());
		dartFn->SetAnalyzedData(std::make_unique<AnalyzedFnData>(app, *dartFn, convertAsm(asm_insns)));
		asm2il(dartFn, asm_insns);
	};
	const bool timing = std::getenv("BLUTTER_TIMING") != nullptr;
	const auto tDisasm = std::chrono::steady_clock::now();

	const auto recordFailure = [&](DartFunction* dartFn, const std::exception& e) {
		++failed;
		failures.push_back(std::format("[!] analysis failed at {:#x} {}: {}",
			dartFn->Address(), dartFn->FullName(), e.what()));
	};

	if (jobs <= 1) {
		for (auto* dartFn : fns) {
			try {
				convertOne(dartFn);
				++analyzed;
			}
			catch (std::exception& e) {
				if (strict)
					throw;
				recordFailure(dartFn, e);
			}
		}
	}
	else {
		const size_t chunkSize = (size_t)std::max(8, jobs * 4);
		for (size_t begin = 0; begin < fns.size(); begin += chunkSize) {
			const size_t end = std::min(fns.size(), begin + chunkSize);
			const size_t n = end - begin;

			std::vector<std::optional<AsmInstructions>> asms(n);
			std::vector<std::exception_ptr> disasmErrors(n);
			std::atomic<size_t> next{ 0 };
			const auto worker = [&]() {
				Disassembler dis;
				for (;;) {
					const size_t i = next.fetch_add(1);
					if (i >= n)
						return;
					auto* dartFn = fns[begin + i];
					try {
						asms[i].emplace(dis.Disasm((uint8_t*)dartFn->MemAddress(), dartFn->Size(), dartFn->Address()));
					}
					catch (...) {
						disasmErrors[i] = std::current_exception();
					}
				}
			};

			std::vector<std::thread> threads;
			const int workerCount = (int)std::min<size_t>((size_t)jobs, n);
			threads.reserve(workerCount - 1);
			for (int t = 1; t < workerCount; t++)
				threads.emplace_back(worker);
			worker();
			for (auto& th : threads)
				th.join();

			for (size_t i = 0; i < n; i++) {
				auto* dartFn = fns[begin + i];
				try {
					if (disasmErrors[i])
						std::rethrow_exception(disasmErrors[i]);
					dartFn->SetAnalyzedData(std::make_unique<AnalyzedFnData>(app, *dartFn, convertAsm(*asms[i])));
					asm2il(dartFn, *asms[i]);
					asms[i].reset(); // release capstone memory as soon as possible
					++analyzed;
				}
				catch (std::exception& e) {
					if (strict)
						throw;
					recordFailure(dartFn, e);
				}
			}
		}
	}

	if (timing)
		std::cerr << "disasm+il: " << std::chrono::duration<double>(std::chrono::steady_clock::now() - tDisasm).count() << "s\n";
	for (auto& msg : failures)
		std::cerr << msg << "\n";

	std::cout << "Analyzed " << analyzed << " functions, " << failed << " failed\n";
}

#endif // NO_CODE_ANALYSIS

std::string FnParamInfo::ToString() const
{
	std::string txt = type ? type->ToString() : "dynamic";
	txt += ' ';
	txt += name.empty() ? "_" : name;
	if (val) {
		txt += " = ";
		txt += val->ToString();
	}
	if (valReg.IsSet() || localOffset) {
		txt += " /* ";
		if (paramReg.IsSet()) {
			txt += paramReg.Name();
			txt += " => ";
		}
		if (valReg.IsSet()) {
			txt += valReg.Name();
			if (localOffset)
				txt += ", ";
		}
		if (localOffset)
			txt += std::format("fp-{:#x}", -localOffset);
		txt += " */";
	}
	return txt;
}

FnParamInfo* FnParams::findValReg(A64::Register reg)
{
	for (auto& param : params) {
		if (param.valReg == reg) {
			return &param;
		}
	}
	return nullptr;
}

bool FnParams::movValReg(A64::Register dstReg, A64::Register srcReg)
{
	for (auto& param : params) {
		if (param.valReg == srcReg) {
			param.valReg = dstReg;
			return true;
		}
	}
	return false;
}

std::string FnParams::ToString() const
{
	std::string txt;
	for (int i = 0; i < params.size(); i++) {
		if (i != 0)
			txt += ", ";
		if (i == numFixedParam)
			txt += isNamedParam ? '{' : '[';

		txt += params[i].ToString();
	}

	if (numFixedParam < params.size())
		txt += isNamedParam ? '}' : ']';

	return txt;
}
