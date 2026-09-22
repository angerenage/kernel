#pragma once

#include <stdint.h>

/* Register state saved by the LoongArch exception entry path. */
struct exception_frame {
	uint64_t gpr[32];
	uint64_t estat;
	uint64_t era;
	uint64_t badv;
	uint64_t prmd;
};
