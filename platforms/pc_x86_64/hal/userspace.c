#include <hal/userspace.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "interrupts_private.h"

#define X86_USER_RFLAGS 0x202ull

enum {
	X86_USER_ENTRY_FRAME_WORDS = 5u,
	X86_THREAD_CTX_R12         = 2,
};

enum {
	X86_IRET_RIP = 0,
	X86_IRET_CS,
	X86_IRET_RFLAGS,
	X86_IRET_RSP,
	X86_IRET_SS,
};

extern void x86_64_userspace_enter(void);

_Static_assert(HAL_CPU_THREAD_CONTEXT_SPILL_WORDS > X86_THREAD_CTX_R12, "x86_64 userspace spill area is too small");

bool hal_userspace_thread_context_init(struct thread_context* context, uintptr_t kernel_stack_top, uintptr_t user_entry,
                                       uintptr_t user_stack, uintptr_t user_arg) {
	uintptr_t* frame;
	uintptr_t  frame_sp;

	if (context == NULL || kernel_stack_top == 0u || user_entry == 0u || user_stack == 0u) return false;
	if (kernel_stack_top < X86_USER_ENTRY_FRAME_WORDS * sizeof(uintptr_t)) return false;

	frame_sp = kernel_stack_top - X86_USER_ENTRY_FRAME_WORDS * sizeof(uintptr_t);
	frame    = (uintptr_t*)frame_sp;

	frame[X86_IRET_RIP]    = user_entry;
	frame[X86_IRET_CS]     = X86_GDT_USER_CODE_SELECTOR | 0x3u;
	frame[X86_IRET_RFLAGS] = X86_USER_RFLAGS;
	frame[X86_IRET_RSP]    = user_stack;
	frame[X86_IRET_SS]     = X86_GDT_USER_DATA_SELECTOR | 0x3u;

	memset(context, 0, sizeof(*context));
	context->instruction_pointer       = (uintptr_t)x86_64_userspace_enter;
	context->stack_pointer             = frame_sp;
	context->spill[X86_THREAD_CTX_R12] = user_arg;
	return true;
}
