#include <hal/userspace.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "interrupts_private.h"

#define X86_USER_RFLAGS 0x202ull
#define X86_RFLAGS_DIRECTION (1ull << 10)

enum {
	X86_USER_ENTRY_FRAME_WORDS     = 5u,
	X86_USER_CALL_RETURN_SLOT_SIZE = sizeof(uintptr_t),
	X86_THREAD_CTX_R12             = 2,
};

enum {
	X86_IRET_RIP = 0,
	X86_IRET_CS,
	X86_IRET_RFLAGS,
	X86_IRET_RSP,
	X86_IRET_SS,
};

extern void x86_64_userspace_enter(void);

#define HAL_USERSPACE_CONTEXT_MAGIC 0x757063616c6c7631ull

struct arch_userspace_context {
	uint64_t                    magic;
	struct user_interrupt_frame frame;
};

_Static_assert(HAL_CPU_THREAD_CONTEXT_SPILL_WORDS > X86_THREAD_CTX_R12, "x86_64 userspace spill area is too small");
_Static_assert(sizeof(struct arch_userspace_context) <= HAL_USERSPACE_INTEGER_CONTEXT_SIZE,
               "x86_64 integer userspace context storage is too small");
_Static_assert(offsetof(struct user_interrupt_frame, frame.rdx) == 24u, "x86_64 userspace rdx offset mismatch");
_Static_assert(offsetof(struct user_interrupt_frame, frame.rdi) == 40u, "x86_64 userspace rdi offset mismatch");
_Static_assert(offsetof(struct user_interrupt_frame, frame.rsi) == 48u, "x86_64 userspace rsi offset mismatch");
_Static_assert(offsetof(struct user_interrupt_frame, frame.rip) == 136u, "x86_64 userspace rip offset mismatch");
_Static_assert(offsetof(struct user_interrupt_frame, rsp) == 160u, "x86_64 userspace rsp offset mismatch");
_Static_assert(sizeof(struct user_interrupt_frame) == 176u, "x86_64 userspace frame size mismatch");

static bool x86_64_frame_is_user(const struct user_interrupt_frame* frame) {
	const uint64_t user_cs = X86_GDT_USER_CODE_SELECTOR | 0x3u;
	const uint64_t user_ss = X86_GDT_USER_DATA_SELECTOR | 0x3u;

	if (frame == NULL || frame->frame.cs != user_cs) return false;
	return frame->ss == user_ss;
}

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
	hal_cpu_fp_context_init(&context->fp_context);
	return true;
}

bool hal_userspace_frame_is_user(const struct hal_userspace_return_frame* frame) {
	return x86_64_frame_is_user((const struct user_interrupt_frame*)frame);
}

bool hal_userspace_context_save(struct hal_userspace_context* context, const struct hal_userspace_return_frame* frame) {
	const struct user_interrupt_frame* native = (const struct user_interrupt_frame*)frame;
	struct arch_userspace_context      saved;

	if (context == NULL || !x86_64_frame_is_user(native)) return false;
	saved = (struct arch_userspace_context){
		.magic = HAL_USERSPACE_CONTEXT_MAGIC,
		.frame = *native,
	};
	memset(context, 0, sizeof(*context));
	memcpy(context->opaque, &saved, sizeof(saved));
	hal_cpu_fp_context_save(&context->fp_context);
	return true;
}

bool hal_userspace_context_restore(struct hal_userspace_return_frame*  frame,
                                   const struct hal_userspace_context* context) {
	struct user_interrupt_frame*  native = (struct user_interrupt_frame*)frame;
	struct arch_userspace_context saved;

	if (context == NULL || !x86_64_frame_is_user(native)) return false;
	memcpy(&saved, context->opaque, sizeof(saved));
	if (saved.magic != HAL_USERSPACE_CONTEXT_MAGIC || !x86_64_frame_is_user(&saved.frame)) return false;
	hal_cpu_fp_context_restore(&context->fp_context);
	*native = saved.frame;
	return true;
}

bool hal_userspace_frame_redirect(struct hal_userspace_return_frame* frame, uintptr_t entry, uintptr_t stack_top,
                                  uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3) {
	struct user_interrupt_frame* native = (struct user_interrupt_frame*)frame;

	if (!x86_64_frame_is_user(native) || entry == 0u || stack_top == 0u) return false;
	if ((stack_top & (HAL_USERSPACE_STACK_ALIGNMENT - 1u)) != 0u) return false;
	if (stack_top < X86_USER_CALL_RETURN_SLOT_SIZE) return false;

	native->frame.rip = entry;
	native->rsp       = stack_top - X86_USER_CALL_RETURN_SLOT_SIZE;
	native->frame.rdi = arg0;
	native->frame.rsi = arg1;
	native->frame.rdx = arg2;
	native->frame.rcx = arg3;
	/* A direct IRET is not a CALL, so establish the SysV function-entry state explicitly. */
	native->frame.rflags &= ~X86_RFLAGS_DIRECTION;
	return true;
}
