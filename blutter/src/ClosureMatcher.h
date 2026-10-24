#pragma once

// Classifies closure prologue displacements against the layout of the Dart version
// this binary was built for. Kept separate from ClosureLayout.h so the pure index
// model stays testable without Dart headers.
#include "ClosureLayout.h"
#include "vm/globals.h"
#include "vm/raw_object.h"
#include "vm/compiler/runtime_offsets_extracted.h"

namespace blutter::closure_layout {

// true when `disp` (relative to the tagged closure pointer) loads the closure context.
// Dart 3.13 bakes an elements[] index instead of a fixed field offset, so the answer
// depends on whether the function declares its own type parameters.
inline bool IsClosureContextDisp(int64_t disp, bool has_delayed_type_args)
{
#ifdef BLUTTER_DART_SINGLE_SNAPSHOT
	const int idx = ElementIndex(
		disp + dart::kHeapObjectTag,
		AOT_Closure_elements_start_offset,
		AOT_Closure_element_size);
	return IsContextElement(idx, has_delayed_type_args);
#else
	(void)has_delayed_type_args;
	return IsFixedFieldDisp(disp, AOT_Closure_context_offset, dart::kHeapObjectTag);
#endif
}

inline bool IsClosureDelayedTypeArgsDisp(int64_t disp)
{
#ifdef BLUTTER_DART_SINGLE_SNAPSHOT
	const int idx = ElementIndex(
		disp + dart::kHeapObjectTag,
		AOT_Closure_elements_start_offset,
		AOT_Closure_element_size);
	return IsDelayedTypeArgsElement(idx);
#else
	return IsFixedFieldDisp(
		disp, AOT_Closure_delayed_type_arguments_offset, dart::kHeapObjectTag);
#endif
}

} // namespace blutter::closure_layout
