#include "pch.h"
#include "il.h"
#include "CodeAnalyzer.h"
#include "DartThreadInfo.h"

std::string SetupParametersInstr::ToString()
{
	return "SetupParameters(" + params->ToString() + ")";
}

std::string CallLeafRuntimeInstr::ToString()
{
	const auto& name = GetThreadOffsetName(thrOffset);
	const auto info = GetThreadLeafFunction(thrOffset);
	// Windows-target snapshots reference Thread offsets that differ from the
	// linux-built VM's layout; the lookup may miss, so fall back gracefully.
	if (info == nullptr) {
		return std::format("CallRuntime_{}", name);
	}
	return std::format("CallRuntime_{}({}) -> {}", name, info->params, info->returnType);
}