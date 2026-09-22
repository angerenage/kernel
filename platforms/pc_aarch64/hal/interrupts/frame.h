#pragma once

#include <stdint.h>

/* Register state saved by the AArch64 exception entry path. */
struct exception_frame {
	uint64_t x[31];
	uint64_t vector;
	uint64_t esr;
	uint64_t far;
	uint64_t elr;
	uint64_t spsr;
	uint64_t sp_el0;
	uint64_t reserved;
};
