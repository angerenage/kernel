#pragma once

#include <base/cap.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Process environment copied onto the main thread's user stack.
 *
 * capv_offset describes capc positional capability IDs. The first four are
 * stdin, stdout, stderr, and the working directory respectively. The C runtime
 * installs them as process state and removes them from the capability arguments
 * exposed to main(). Invalid IDs represent unavailable standard resources.
 * argv_offset and argv_size describe exactly argc consecutive NUL-terminated
 * strings. Offsets are relative to this structure and zero for empty lists.
 * Application capability IDs, including invalid IDs, retain their positional order.
 */

enum process_startup_capability_index {
	PROCESS_STARTUP_CAP_STDIN = 0u,
	PROCESS_STARTUP_CAP_STDOUT,
	PROCESS_STARTUP_CAP_STDERR,
	PROCESS_STARTUP_CAP_WORKING_DIRECTORY,
	PROCESS_STARTUP_CAP_COUNT,
};

struct process_startup_info {
	uint32_t  size;
	uintptr_t heap_base;
	size_t    heap_size;
	cap_id_t  memory_allocator_cap;
	cap_id_t  registry_cap;
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
	uint32_t  argc;
	uint32_t  argv_offset;
	uint32_t  argv_size;
};
