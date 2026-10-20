#pragma once
#include <cstdint>

// Dart 3.13 replaced Closure's fixed `context` / `delayed_type_arguments` fields with a
// variable-length `elements[]` region. Field order (Closure::ContextIndex in object.h):
//   [delayed type args][instantiator type args][parent function type args][context][captures]
// Every present type-argument slot pushes the context one element further, so the context
// index equals the number of present slots.
//
// blutter recovers only "the function declares its own type parameters" (delayed type
// args). The instantiator and parent-function slots are not modelled, so the context check
// accepts an index range: 0 when the function is not generic, 1..3 otherwise.
namespace blutter::closure_layout {

constexpr int kNoElement = -1;
// Highest index the context can occupy: delayed + instantiator + function type args.
constexpr int kMaxContextIndex = 3;

// Untagged element index of a field offset, or kNoElement when the offset is not aligned
// inside the modelled part of the region (index > kMaxContextIndex means captures).
constexpr int ElementIndex(int64_t field_offset, int64_t elements_start, int64_t element_size)
{
	if (element_size <= 0 || field_offset < elements_start)
		return kNoElement;
	const int64_t delta = field_offset - elements_start;
	if (delta % element_size != 0)
		return kNoElement;
	const int64_t idx = delta / element_size;
	if (idx > kMaxContextIndex)
		return kNoElement;
	return static_cast<int>(idx);
}

// Delayed type arguments always sit at elements[0] when the function is generic.
constexpr bool IsDelayedTypeArgsElement(int idx)
{
	return idx == 0;
}

constexpr bool IsContextElement(int idx, bool has_delayed_type_args)
{
	if (idx == kNoElement)
		return false;
	const int min_idx = has_delayed_type_args ? 1 : 0;
	return idx >= min_idx && idx <= kMaxContextIndex;
}

// Pre-3.13 (and any layout with a fixed field offset): the prologue bakes an absolute
// byte offset relative to the tagged closure pointer, hence the tag compensation.
constexpr bool IsFixedFieldDisp(int64_t disp, int64_t field_offset, int64_t heap_object_tag)
{
	return disp == field_offset - heap_object_tag;
}

} // namespace blutter::closure_layout
