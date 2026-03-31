#pragma once

#include <stdbool.h>
#include <stdint.h>

struct exception_frame {
	uint64_t gpr[32];
	uint64_t estat;
	uint64_t era;
	uint64_t badv;
	uint64_t reserved;
};

bool clock_handle_irq(const struct exception_frame* frame);
