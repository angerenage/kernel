#pragma once

#include <base/cap.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Process environment copied onto the main thread's user stack.
 *
 * capv_offset describes capc positional capability IDs.
 * argv_offset and argv_size describe exactly argc consecutive NUL-terminated
 * strings. Offsets are relative to this structure and zero for empty lists.
 * Capability IDs, including invalid IDs, are passed through without interpretation.
 */
struct process_startup_info {
	uint32_t  size;
	uintptr_t heap_base;
	size_t    heap_size;
	cap_id_t  memory_allocator_cap;
	cap_id_t  serial_cap;
	cap_id_t  init_cap;
	uint32_t  capc;
	uint32_t  capv_offset;
	uint32_t  argc;
	uint32_t  argv_offset;
	uint32_t  argv_size;
};

/* Minimal process environment passed by the kernel specifically to init. */
struct init_startup_info {
	uint32_t  size;
	uintptr_t heap_base;
	size_t    heap_size;
	cap_id_t  memory_allocator_cap;
	cap_id_t  kernel_resources_cap;
};
