#pragma once

#include <base/cap.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Process environment copied onto the main thread's user stack.
 *
 * argv_offset and argv_size describe a region containing exactly argc
 * consecutive NUL-terminated strings. argv_offset is relative to
 * the beginning of this structure. argc == 0 requires both values to be zero.
 */
struct process_startup_info {
	uint32_t  size;
	uintptr_t heap_base;
	size_t    heap_page_count;
	size_t    page_size;
	cap_id_t  serial_cap;
	cap_id_t  init_cap;
	uint32_t  argc;
	uint32_t  argv_offset;
	uint32_t  argv_size;
};

/* Minimal process environment passed by the kernel specifically to init. */
struct init_startup_info {
	uint32_t  size;
	uintptr_t heap_base;
	size_t    heap_page_count;
	size_t    page_size;
	cap_id_t  kernel_resources_cap;
};
