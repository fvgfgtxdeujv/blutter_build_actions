#include "pch.h"
#include "DartLoader.h"
#include <stdexcept>
#include <cstdlib>
#include <type_traits>
#include <utility>

// Note: most running dart VM code from runtime/bin/main.cc

static void init_vm_flags()
{
	// From flutter/engine/runtime/dart_vm.cc
	const char* options[] = {
		//"--ignore-unrecognized-flags",
		//"--enable_mirrors=false",
		"--precompilation",
	};
	char* error = Dart_SetVMFlags(sizeof(options) / sizeof(*options), options);
	if (error)
		throw std::runtime_error(error);
}

// Dart 3.13 removed vm_snapshot_data/instructions from Dart_InitializeParams (the VM snapshot is
// now built into the runtime). This trait lets the assignment below compile conditionally, so the
// same source works for both Dart < 3.13 and >= 3.13.
template <typename P, typename = void>
struct has_vm_snapshot_params : std::false_type {};
template <typename P>
struct has_vm_snapshot_params<P, std::void_t<decltype(std::declval<P&>().vm_snapshot_data)>> : std::true_type {};

template <typename P>
static void set_vm_snapshot_params(P& params, const uint8_t* data, const uint8_t* instr)
{
	if constexpr (has_vm_snapshot_params<P>::value) {
		params.vm_snapshot_data = data;
		params.vm_snapshot_instructions = instr;
	}
	else {
		(void)data;
		(void)instr;
	}
}

static void init_dart(const uint8_t* vm_snapshot_data, const uint8_t* vm_snapshot_instructions)
{
	char* error = NULL;

	Dart_InitializeParams init_params;
	memset(&init_params, 0, sizeof(init_params));
	init_params.version = DART_INITIALIZE_PARAMS_CURRENT_VERSION;
	set_vm_snapshot_params(init_params, vm_snapshot_data, vm_snapshot_instructions);
	init_params.start_kernel_isolate = false;
	// other params are no needed if snapshot is not run
	error = Dart_Initialize(&init_params);
	if (error) {
		throw std::runtime_error(error);
	}
}

static Dart_Isolate load_isolate(const uint8_t* isolate_snapshot_data, const uint8_t* isolate_snapshot_instructions)
{
	char* error = NULL;

	Dart_IsolateFlags flags;
	Dart_IsolateFlagsInitialize(&flags);
	flags.is_system_isolate = false;
	//flags.snapshot_is_dontneed_safe = true; // Dart <= 2.14 has no this field
	// dart 3 is always null safety
	// null safety is enabled by default on Flutter 2.0 with Dart 2.12 (since April 2021)
	// null safety flag is removed in https://github.com/dart-lang/sdk/commit/8e2acda7d68702d43eefae0c7b17d314d5c93b00#diff-0c27ee5540bcadf9531563ffd7dc5266b1475db16c6d75b73949611551452348
	auto pos = strstr((const char*)isolate_snapshot_data + 0x30, "null-safety");
	if (pos == NULL) {
		// the null-safety flag is removed because it is always enabled.
		flags.null_safety = true;
	}
	else {
		// "no-null-safety" is set when null safety is disabled. So check for space
		flags.null_safety = pos[-1] == ' ';
	}

	auto isolate = Dart_CreateIsolateGroup(nullptr, nullptr, isolate_snapshot_data,
		isolate_snapshot_instructions, &flags,
		nullptr, nullptr, &error);
	if (isolate == NULL) {
		throw std::runtime_error(error);
	}

	return isolate;
}

Dart_Isolate DartLoader::Load(LibAppInfo& libInfo)
{
	init_vm_flags();

	init_dart(libInfo.vm_snapshot_data, libInfo.vm_snapshot_instructions);

	auto isolate = load_isolate(libInfo.isolate_snapshot_data, libInfo.isolate_snapshot_instructions);

	return isolate;
}

template<typename T>
inline void ignore_result(const T& /* unused result */) {}

void DartLoader::Unload()
{
	if (Dart_CurrentIsolate() != nullptr) {
		Dart_ShutdownIsolate();
	}
	ignore_result(Dart_Cleanup());
}
