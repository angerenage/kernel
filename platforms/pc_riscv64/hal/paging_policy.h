#pragma once

#include <stdbool.h>

#include <base/memory.h>

static inline bool riscv_paging_memory_type_supported(enum memory_type memory_type, bool svpbmt_available) {
	if (memory_type == MEMORY_TYPE_NORMAL) return true;
	if (memory_type == MEMORY_TYPE_DEVICE || memory_type == MEMORY_TYPE_WRITE_COMBINING) return svpbmt_available;
	return false;
}
