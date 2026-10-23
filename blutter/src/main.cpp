#include "pch.h"
#include "DartApp.h"
#include "DartDumper.h"
#include "CodeAnalyzer.h"
#include "FridaWriter.h"
#include "JsonDumper.h"
#include "PseudoCode.h"
#include "args.hxx"
#include <filesystem>
#include <thread>

int main(int argc, char** argv)
{
	// stdout/stderr are block-buffered when redirected to a pipe/file: if the process is killed
	// (OOM/timeout) the buffered output is lost, making the log appear to stop mid-line. Disable
	// buffering so progress is visible in real time.
	setvbuf(stdout, nullptr, _IONBF, 0);
	setvbuf(stderr, nullptr, _IONBF, 0);

	args::ArgumentParser parser("B(l)utter - Reversing flutter application", "");
	args::HelpFlag help(parser, "help", "Display this help menu", { 'h', "help" });
	args::Group reqGrp(parser, "Required arguments", args::Group::Validators::All);
	args::ValueFlag<std::string> infile(reqGrp, "infile", "libapp file", { 'i', "in" });
	args::ValueFlag<std::string> outdir(reqGrp, "outdir", "out path", { 'o', "out"});
	args::ValueFlag<std::string> blacklist(parser, "blacklist", "semantic blacklist file (default: blutter/src/semantic_blacklist.txt)", { 'b', "blacklist"});
	args::Flag pseudo(parser, "pseudo", "also emit pseudo-code comment blocks in asm output (off by default)", { 'p', "pseudo" });
	args::Flag exception(parser, "exception", "also emit exception try/handler markers in asm output (off by default)", { 'e', "exception" });
	args::Flag types(parser, "types", "also emit recovered return-type annotations at call sites (off by default)", { 't', "types" });
	args::Flag json(parser, "json", "also write blutter_dump.json (structured library/class/function export)", { 'j', "json" });
	args::Flag jsonSplit(parser, "json-split", "with --json, write one file per library under blutter_dump.json.d/ and keep the main file as an index", { "json-split" });
	args::Flag strict(parser, "strict", "abort on the first function that fails analysis (default: log and continue)", { "strict" });
	args::ValueFlag<int> jobs(parser, "N", "disassemble with N worker threads (0 = auto, default 1)", { "jobs" }, 1);

	try {
		parser.ParseCLI(argc, argv);
		if (blacklist)
			SetSemanticBlacklistFile(args::get(blacklist));
		if (pseudo)
			PseudoCode::SetEnabled(true);
		if (exception)
			SetExceptionViewEnabled(true);
		if (types)
			SetTypeSymbolViewEnabled(true);

		auto& libappPath = args::get(infile);

		std::filesystem::path outDir{ args::get(outdir) };
		std::error_code ec;
		if (!std::filesystem::create_directory(outDir, ec) && ec.value() != 0) {
			std::cerr << "Failed to create output directory: " << ec.message() << "\n";
			return 1;
		}

		DartApp app{ libappPath.c_str() };
		std::cout << std::format("libapp is loaded at {:#x}\n", app.base());
		std::cout << std::format("Dart heap at {:#x}\n", app.heap_base());

		app.EnterScope();
		app.LoadInfo();
		app.ExitScope();

		app.EnterScope();
		// Stage timing for profiling; opt-in via BLUTTER_TIMING=1.
		const bool timing = std::getenv("BLUTTER_TIMING") != nullptr;
		auto t0 = std::chrono::steady_clock::now();
		const auto lap = [&](const char* what) {
			if (!timing)
				return;
			auto now = std::chrono::steady_clock::now();
			std::cerr << what << ": " << std::chrono::duration<double>(now - t0).count() << "s\n";
			t0 = now;
		};
#ifndef NO_CODE_ANALYSIS
		std::cout << "Analyzing the application\n";
		CodeAnalyzer analyzer{ app };
		int jobCount = jobs ? args::get(jobs) : 1;
		if (jobCount <= 0) {
			const auto hw = std::thread::hardware_concurrency();
			jobCount = (int)std::clamp<unsigned>(hw == 0 ? 1 : hw, 1, 8);
		}
		if (jobCount > 1)
			std::cout << "Analyzing with " << jobCount << " threads\n";
		analyzer.AnalyzeAll(static_cast<bool>(strict), jobCount);
		// Fill return/parameter types dropped from the snapshot using the IL
		// analysis result (opt-in output stays behind -t/--types).
		app.BackfillSignaturesFromAnalysis();
#endif
		lap("analysis");

		DartDumper dumper{ app };
		std::cout << "Dumping Object Pool\n";
		dumper.DumpObjectPool((outDir / "pp.txt").string().c_str());
		dumper.DumpObjects((outDir / "objs.txt").string().c_str());
		lap("pool+objects");
		if (json || jsonSplit) {
			std::cout << "Writing JSON dump\n";
			JsonDumper jsonDumper{ app };
			jsonDumper.Write((outDir / "blutter_dump.json").string().c_str(), static_cast<bool>(jsonSplit));
		}
		lap("json");
#ifndef NO_CODE_ANALYSIS
		std::cout << "Generating application assemblies\n";
#else
		std::cout << "Generating application functions in asm folder\n";
#endif
		dumper.DumpCode((outDir / "asm").string().c_str());
		lap("asm");
		dumper.DumpStringCrossRef((outDir / "strings_to_funcs.txt").string().c_str());
		dumper.Dump4Ida(outDir / "ida_script");
		lap("strings+ida");

		std::cout << "Generating Frida script\n";
		FridaWriter fwriter{ app };
#ifdef TARGET_ARCH_X64
		// Windows 桌面（x64）目标：frida 附加到运行中的 Windows 应用。
		// 对象模型按非压缩指针实现，镜像基址由函数锚点探测；基址探测阈值
		// 与运行时行为需在真实 Windows + frida 环境复核
		fwriter.Create((outDir / "blutter_frida_windows.js").string().c_str());
#else
		// Android 手机端 hook 运行中的 Flutter 应用
		fwriter.Create((outDir / "blutter_frida.js").string().c_str());
#endif

		app.ExitScope();
	}
	catch (args::Help&) {
		std::cout << parser;
		return 0;
	}
	catch (args::ParseError& e) {
		std::cerr << e.what() << "\n";
		std::cerr << parser;
		return 1;
	}
	catch (args::ValidationError& e) {
		std::cerr << e.what() << "\n";
		std::cerr << parser;
		return 1;
	}
	catch (std::exception& e) {
		std::cerr << "exception: " << e.what() << "\n";
	}

	return 0;
}