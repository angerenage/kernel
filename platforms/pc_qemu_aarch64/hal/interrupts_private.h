#pragma once

#include <stdbool.h>
#include <stdint.h>

struct exception_frame {
	uint64_t x[31];
	uint64_t vector;
	uint64_t esr;
	uint64_t far;
	uint64_t elr;
	uint64_t spsr;
};

bool clock_handle_irq(const struct exception_frame* frame);
