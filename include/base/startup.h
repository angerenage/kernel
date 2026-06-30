#pragma once

#include <base/cap.h>
#include <stddef.h>
#include <stdint.h>

/* Process environment copied onto the main thread's user stack. */
struct process_startup_info {
	uint32_t  size;
	uintptr_t heap_base;
	size_t    heap_page_count;
	size_t    page_size;
	cap_id_t  serial_cap;
};
