// Unit tests for the Dart 3.13 closure elements[] index model (see ClosureLayout.h).
// Plain asserts + exit code so the test needs no test framework and no Dart headers.
#include "../src/ClosureLayout.h"
#include <cstdint>
#include <cstdio>

namespace cl = blutter::closure_layout;

static int failures = 0;

#define CHECK(cond)                                                              \
	do {                                                                         \
		if (!(cond)) {                                                           \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
			++failures;                                                          \
		}                                                                        \
	} while (0)

// Compile-time sanity: the analyzer relies on these being constexpr.
static_assert(cl::ElementIndex(0x14, 0x14, 4) == 0);
static_assert(cl::ElementIndex(0x14 + 12, 0x14, 4) == 3);
static_assert(cl::ElementIndex(0x14 + 16, 0x14, 4) == cl::kNoElement);
static_assert(cl::IsContextElement(0, false));
static_assert(!cl::IsContextElement(0, true));

int main()
{
	// Real offsets from runtime_offsets_extracted.h: AOT_Closure_elements_start_offset
	// is 0x14 with compressed pointers and 0x28 without.
	const int64_t starts[] = { 0x14, 0x28 };
	const int64_t sizes[] = { 4, 8 };

	for (int64_t start : starts) {
		for (int64_t size : sizes) {
			// elements[0..3] map to indices 0..3, captures (index >= 4) do not.
			for (int idx = 0; idx <= cl::kMaxContextIndex; ++idx)
				CHECK(cl::ElementIndex(start + idx * size, start, size) == idx);
			CHECK(cl::ElementIndex(start + 4 * size, start, size) == cl::kNoElement);

			// Misaligned / below-start / non-positive element size are rejected.
			CHECK(cl::ElementIndex(start + 1, start, size) == cl::kNoElement);
			CHECK(cl::ElementIndex(start - size, start, size) == cl::kNoElement);
			CHECK(cl::ElementIndex(start, start, 0) == cl::kNoElement);
		}
	}

	// Delayed type args are always elements[0].
	CHECK(cl::IsDelayedTypeArgsElement(0));
	CHECK(!cl::IsDelayedTypeArgsElement(1));
	CHECK(!cl::IsDelayedTypeArgsElement(cl::kNoElement));

	// Context: index 0 without delayed type args, 1..3 with them (instantiator and
	// parent-function type args are unmodelled, hence the range).
	CHECK(cl::IsContextElement(0, false));
	CHECK(cl::IsContextElement(1, false));
	CHECK(cl::IsContextElement(3, false));
	CHECK(!cl::IsContextElement(0, true));
	CHECK(cl::IsContextElement(1, true));
	CHECK(cl::IsContextElement(3, true));
	CHECK(!cl::IsContextElement(4, true));
	CHECK(!cl::IsContextElement(cl::kNoElement, false));
	CHECK(!cl::IsContextElement(cl::kNoElement, true));

	// Pre-3.13 layout: the prologue stores field_offset - kHeapObjectTag.
	const int64_t tag = 1;
	const int64_t context_offset = 0x28;
	CHECK(cl::IsFixedFieldDisp(context_offset - tag, context_offset, tag));
	CHECK(!cl::IsFixedFieldDisp(context_offset, context_offset, tag));
	CHECK(!cl::IsFixedFieldDisp(-1, context_offset, tag));

	if (failures == 0)
		std::printf("closure_layout: all checks passed\n");
	return failures == 0 ? 0 : 1;
}
