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
	uint64_t sp_el0;
	uint64_t reserved;
};

void aarch64_prepare_user_return(void);

bool clock_handle_irq(const struct exception_frame* frame);
bool aarch64_handle_syscall(struct exception_frame* frame, uint64_t ec);
