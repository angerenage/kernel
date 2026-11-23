#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <base/math.h>

static inline bool paging_virtual_range_end(uintptr_t start, size_t size, unsigned address_bits, uintptr_t* out_end) {
	uint64_t end;
	uint64_t last;
	uint64_t upper_mask;
	uint64_t sign_bit;

	if (size == 0u || out_end == NULL || address_bits == 0u || address_bits > 64u ||
			add_overflow_u64(start, size, &end) || end > UINTPTR_MAX)
		return false;
	last = end - 1u;
	if (address_bits != 64u) {
		upper_mask = UINT64_MAX << address_bits;
		sign_bit = 1ull << (address_bits - 1u);
		if (((uint64_t)start & upper_mask) != (((uint64_t)start & sign_bit) != 0u ? upper_mask : 0u) ||
				(last & upper_mask) != ((last & sign_bit) != 0u ? upper_mask : 0u) ||
				(((uint64_t)start ^ last) & sign_bit) != 0u)
			return false;
	}
	*out_end = (uintptr_t)end;
	return true;
}

static inline bool paging_split_virtual_range_end(
		uintptr_t start, size_t size, unsigned address_bits, bool upper, uintptr_t* out_end) {
	uint64_t end;
	uint64_t last;
	uint64_t upper_mask;
	uint64_t expected_upper;

	if (size == 0u || out_end == NULL || address_bits == 0u || address_bits > 64u ||
			add_overflow_u64(start, size, &end) || end > UINTPTR_MAX)
		return false;
	last = end - 1u;
	if (address_bits != 64u) {
		upper_mask = UINT64_MAX << address_bits;
		expected_upper = upper ? upper_mask : 0u;
		if (((uint64_t)start & upper_mask) != expected_upper || (last & upper_mask) != expected_upper) return false;
	}
	else if (upper) return false;
	*out_end = (uintptr_t)end;
	return true;
}
