#include <hal/userspace.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "interrupts_private.h"

#if defined(__riscv_compressed)
#define RISCV64_USER_INSTRUCTION_ALIGNMENT 2u
#else
#define RISCV64_USER_INSTRUCTION_ALIGNMENT 4u
#endif

enum {
	RISCV64_USER_STACK_ALIGNMENT = 16u,
	RISCV64_SSTATUS_SPP          = 1u << 8,
	RISCV64_THREAD_CTX_S0        = 0,
	RISCV64_THREAD_CTX_S1,
	RISCV64_THREAD_CTX_S2,
};

extern void riscv64_userspace_enter(void);

#define HAL_USERSPACE_CONTEXT_MAGIC 0x757063616c6c7631ull

struct arch_userspace_context {
	uint64_t               magic;
	struct exception_frame frame;
};

_Static_assert(HAL_CPU_THREAD_CONTEXT_SPILL_WORDS > RISCV64_THREAD_CTX_S2, "riscv64 userspace spill area is too small");
_Static_assert(sizeof(struct arch_userspace_context) <= HAL_USERSPACE_INTEGER_CONTEXT_SIZE,
               "riscv64 integer userspace context storage is too small");
_Static_assert(offsetof(struct exception_frame, sp) == 8u, "riscv64 userspace sp offset mismatch");
_Static_assert(offsetof(struct exception_frame, a0) == 72u, "riscv64 userspace a0 offset mismatch");
_Static_assert(offsetof(struct exception_frame, a1) == 80u, "riscv64 userspace a1 offset mismatch");
_Static_assert(offsetof(struct exception_frame, a2) == 88u, "riscv64 userspace a2 offset mismatch");
_Static_assert(offsetof(struct exception_frame, sepc) == 256u, "riscv64 userspace sepc offset mismatch");
_Static_assert(offsetof(struct exception_frame, sstatus) == 272u, "riscv64 userspace sstatus offset mismatch");
_Static_assert(sizeof(struct exception_frame) == 288u, "riscv64 userspace frame size mismatch");

static bool riscv64_frame_is_user(const struct exception_frame* frame) {
	return frame != NULL && (frame->sstatus & RISCV64_SSTATUS_SPP) == 0u;
}

bool hal_userspace_thread_context_init(struct thread_context* context, uintptr_t kernel_stack_top, uintptr_t user_entry,
                                       uintptr_t user_stack, uintptr_t user_arg) {
	uintptr_t kernel_sp;
	uintptr_t user_sp;

	if (context == NULL || kernel_stack_top == 0u || user_entry == 0u || user_stack == 0u) return false;

	kernel_sp = kernel_stack_top & ~((uintptr_t)RISCV64_USER_STACK_ALIGNMENT - 1u);
	user_sp   = user_stack & ~((uintptr_t)RISCV64_USER_STACK_ALIGNMENT - 1u);
	if (kernel_sp == 0u || user_sp == 0u) return false;

	memset(context, 0, sizeof(*context));
	context->instruction_pointer          = (uintptr_t)riscv64_userspace_enter;
	context->stack_pointer                = kernel_sp;
	context->spill[RISCV64_THREAD_CTX_S0] = user_entry;
	context->spill[RISCV64_THREAD_CTX_S1] = user_arg;
	context->spill[RISCV64_THREAD_CTX_S2] = user_sp;
	hal_cpu_fp_context_init(&context->fp_context);
	return true;
}

bool hal_userspace_frame_is_user(const struct hal_userspace_return_frame* frame) {
	return riscv64_frame_is_user((const struct exception_frame*)frame);
}

bool hal_userspace_context_save(struct hal_userspace_context* context, const struct hal_userspace_return_frame* frame) {
	const struct exception_frame* native = (const struct exception_frame*)frame;
	struct arch_userspace_context saved;

	if (context == NULL || !riscv64_frame_is_user(native)) return false;
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
	struct exception_frame*       native = (struct exception_frame*)frame;
	struct arch_userspace_context saved;

	if (context == NULL || !riscv64_frame_is_user(native)) return false;
	memcpy(&saved, context->opaque, sizeof(saved));
	if (saved.magic != HAL_USERSPACE_CONTEXT_MAGIC || !riscv64_frame_is_user(&saved.frame)) return false;
	hal_cpu_fp_context_restore(&context->fp_context);
	*native = saved.frame;
	return true;
}

bool hal_userspace_frame_redirect(struct hal_userspace_return_frame* frame, uintptr_t entry, uintptr_t stack_top,
                                  uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3) {
	struct exception_frame* native = (struct exception_frame*)frame;

	if (!riscv64_frame_is_user(native) || entry == 0u || stack_top == 0u) return false;
	if ((entry & (RISCV64_USER_INSTRUCTION_ALIGNMENT - 1u)) != 0u) return false;
	if ((stack_top & (HAL_USERSPACE_STACK_ALIGNMENT - 1u)) != 0u) return false;

	native->sepc = entry;
	native->ra   = 0u;
	native->sp   = stack_top;
	native->a0   = arg0;
	native->a1   = arg1;
	native->a2   = arg2;
	native->a3   = arg3;
	return true;
}
