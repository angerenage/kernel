#pragma once

#include <stdint.h>

/* Register state saved by the x86-64 interrupt entry path. */
struct interrupt_frame {
	uint64_t rax;
	uint64_t rbx;
	uint64_t rcx;
	uint64_t rdx;
	uint64_t rbp;
	uint64_t rdi;
	uint64_t rsi;
	uint64_t r8;
	uint64_t r9;
	uint64_t r10;
	uint64_t r11;
	uint64_t r12;
	uint64_t r13;
	uint64_t r14;
	uint64_t r15;
	uint64_t vector;
	uint64_t error_code;
	uint64_t rip;
	uint64_t cs;
	uint64_t rflags;
};

/* Interrupt frame extension saved when an exception crosses privilege levels. */
struct user_interrupt_frame {
	struct interrupt_frame frame;
	uint64_t               rsp;
	uint64_t               ss;
};

/* Safe return mechanisms selected for a validated userspace frame. */
enum x86_user_return_kind {
	X86_USER_RETURN_SYSRET = 0,
	X86_USER_RETURN_IRET,
	X86_USER_RETURN_INVALID,
};
