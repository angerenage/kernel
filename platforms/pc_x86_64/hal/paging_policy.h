#pragma once

#include <stdbool.h>
#include <stdint.h>

#define X86_PAT_UC 0u
#define X86_PAT_WC 1u
#define X86_PAT_WB 6u
#define X86_PAT_ENTRY_COUNT 8u

static inline uint8_t x86_pat_entry(uint64_t pat, uint8_t index) {
	return index < X86_PAT_ENTRY_COUNT ? (uint8_t)(pat >> ((uint64_t)index * 8u)) : UINT8_MAX;
}

static inline bool x86_pat_normal_memory_supported(bool pat_available, uint64_t pat) {
	return !pat_available || x86_pat_entry(pat, 0u) == X86_PAT_WB;
}
