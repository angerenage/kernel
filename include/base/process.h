#pragma once

#include <stdint.h>

typedef uintptr_t process_exit_code_t;

enum process_exit_code {
	PROCESS_EXIT_SUCCESS = 0u,

	PROCESS_EXIT_MEMORY_NOT_PRESENT = 0x100u,
	PROCESS_EXIT_MEMORY_PROTECTION  = 0x101u,
	PROCESS_EXIT_MEMORY_INVALID     = 0x102u,
};
