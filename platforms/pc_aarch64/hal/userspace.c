#include <hal/userspace.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "interrupts_private.h"

enum {
	AARCH64_USER_STACK_ALIGNMENT       = 16u,
	AARCH64_USER_INSTRUCTION_ALIGNMENT = 4u,
	/* EL0t with the exception mask bits clear so timer IRQs can preempt a new userspace thread. */
	AARCH64_USER_SPSR_EL0T = 0x000u,
	AARCH64_SPSR_MODE_MASK = 0x1fu,
	AARCH64_SPSR_MODE_EL0T = 0x00u,
	AARCH64_LINK_REGISTER  = 30u,
	AARCH64_THREAD_CTX_X19 = 0,
	AARCH64_THREAD_CTX_X20,
	AARCH64_THREAD_CTX_X21,
	AARCH64_THREAD_CTX_X22,
};

extern void aarch64_userspace_enter(void);

#define HAL_USERSPACE_CONTEXT_MAGIC 0x757063616c6c7631ull

struct arch_userspace_context {
	uint64_t               magic;
	struct exception_frame frame;
};

_Static_assert(HAL_CPU_THREAD_CONTEXT_SPILL_WORDS > AARCH64_THREAD_CTX_X22,
               "aarch64 userspace spill area is too small");
_Static_assert(sizeof(struct arch_userspace_context) <= HAL_USERSPACE_INTEGER_CONTEXT_SIZE,
               "aarch64 integer userspace context storage is too small");
_Static_assert(offsetof(struct exception_frame, x[0]) == 0u, "aarch64 userspace x0 offset mismatch");
_Static_assert(offsetof(struct exception_frame, elr) == 272u, "aarch64 userspace elr offset mismatch");
_Static_assert(offsetof(struct exception_frame, spsr) == 280u, "aarch64 userspace spsr offset mismatch");
_Static_assert(offsetof(struct exception_frame, sp_el0) == 288u, "aarch64 userspace sp offset mismatch");
_Static_assert(sizeof(struct exception_frame) == 304u, "aarch64 userspace frame size mismatch");

static bool aarch64_frame_is_user(const struct exception_frame* frame) {
	return frame != NULL && (frame->spsr & AARCH64_SPSR_MODE_MASK) == AARCH64_SPSR_MODE_EL0T;
}

bool hal_userspace_thread_context_init(struct thread_context* context, uintptr_t kernel_stack_top, uintptr_t user_entry,
                                       uintptr_t user_stack, uintptr_t user_arg) {
	uintptr_t kernel_sp;
	uintptr_t user_sp;

	if (context == NULL || kernel_stack_top == 0u || user_entry == 0u || user_stack == 0u) return false;

	kernel_sp = kernel_stack_top & ~((uintptr_t)AARCH64_USER_STACK_ALIGNMENT - 1u);
	user_sp   = user_stack & ~((uintptr_t)AARCH64_USER_STACK_ALIGNMENT - 1u);
	if (kernel_sp == 0u || user_sp == 0u) return false;

	memset(context, 0, sizeof(*context));
	context->instruction_pointer           = (uintptr_t)aarch64_userspace_enter;
	context->stack_pointer                 = kernel_sp;
	context->spill[AARCH64_THREAD_CTX_X19] = user_entry;
	context->spill[AARCH64_THREAD_CTX_X20] = user_arg;
	context->spill[AARCH64_THREAD_CTX_X21] = user_sp;
	context->spill[AARCH64_THREAD_CTX_X22] = AARCH64_USER_SPSR_EL0T;
	hal_cpu_fp_context_init(&context->fp_context);
	return true;
}

bool hal_userspace_frame_is_user(const struct hal_userspace_return_frame* frame) {
	return aarch64_frame_is_user((const struct exception_frame*)frame);
}

bool hal_userspace_context_save(struct hal_userspace_context* context, const struct hal_userspace_return_frame* frame) {
	const struct exception_frame* native = (const struct exception_frame*)frame;
	struct arch_userspace_context saved;

	if (context == NULL || !aarch64_frame_is_user(native)) return false;
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

	if (context == NULL || !aarch64_frame_is_user(native)) return false;
	memcpy(&saved, context->opaque, sizeof(saved));
	if (saved.magic != HAL_USERSPACE_CONTEXT_MAGIC || !aarch64_frame_is_user(&saved.frame)) return false;
	hal_cpu_fp_context_restore(&context->fp_context);
	*native = saved.frame;
	return true;
}

bool hal_userspace_frame_redirect(struct hal_userspace_return_frame* frame, uintptr_t entry, uintptr_t stack_top,
                                  uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4) {
	struct exception_frame* native = (struct exception_frame*)frame;

	if (!aarch64_frame_is_user(native) || entry == 0u || stack_top == 0u) return false;
	if ((entry & (AARCH64_USER_INSTRUCTION_ALIGNMENT - 1u)) != 0u) return false;
	if ((stack_top & (HAL_USERSPACE_STACK_ALIGNMENT - 1u)) != 0u) return false;

	native->elr                      = entry;
	native->sp_el0                   = stack_top;
	native->x[AARCH64_LINK_REGISTER] = 0u;
	native->x[0]                     = arg0;
	native->x[1]                     = arg1;
	native->x[2]                     = arg2;
	native->x[3]                     = arg3;
	native->x[4]                     = arg4;
	return true;
}
