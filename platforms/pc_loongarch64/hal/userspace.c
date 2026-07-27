#include <hal/userspace.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "interrupts_private.h"

enum {
	LOONGARCH64_USER_STACK_ALIGNMENT       = 16u,
	LOONGARCH64_USER_INSTRUCTION_ALIGNMENT = 4u,
	LOONGARCH64_PRMD_PPLV_MASK             = 0x3u,
	LOONGARCH64_PRMD_PPLV_USER             = 0x3u,
	LOONGARCH64_GPR_RA                     = 1,
	LOONGARCH64_GPR_SP                     = 3,
	LOONGARCH64_GPR_A0                     = 4,
	LOONGARCH64_GPR_A1                     = 5,
	LOONGARCH64_GPR_A2                     = 6,
	LOONGARCH64_GPR_A3                     = 7,
	LOONGARCH64_THREAD_CTX_S0              = 1,
	LOONGARCH64_THREAD_CTX_S1,
	LOONGARCH64_THREAD_CTX_S2,
};

extern void loongarch64_userspace_enter(void);

#define HAL_USERSPACE_CONTEXT_MAGIC 0x757063616c6c7631ull

struct arch_userspace_context {
	uint64_t               magic;
	struct exception_frame frame;
};

_Static_assert(HAL_CPU_THREAD_CONTEXT_SPILL_WORDS > LOONGARCH64_THREAD_CTX_S2,
               "loongarch64 userspace spill area is too small");
_Static_assert(sizeof(struct arch_userspace_context) <= HAL_USERSPACE_CONTEXT_SIZE,
               "loongarch64 userspace context storage is too small");
_Static_assert(offsetof(struct exception_frame, gpr[LOONGARCH64_GPR_RA]) == 8u,
               "loongarch64 userspace ra offset mismatch");
_Static_assert(offsetof(struct exception_frame, gpr[LOONGARCH64_GPR_SP]) == 24u,
               "loongarch64 userspace sp offset mismatch");
_Static_assert(offsetof(struct exception_frame, gpr[LOONGARCH64_GPR_A0]) == 32u,
               "loongarch64 userspace a0 offset mismatch");
_Static_assert(offsetof(struct exception_frame, gpr[LOONGARCH64_GPR_A1]) == 40u,
               "loongarch64 userspace a1 offset mismatch");
_Static_assert(offsetof(struct exception_frame, gpr[LOONGARCH64_GPR_A2]) == 48u,
               "loongarch64 userspace a2 offset mismatch");
_Static_assert(offsetof(struct exception_frame, era) == 264u, "loongarch64 userspace era offset mismatch");
_Static_assert(offsetof(struct exception_frame, prmd) == 280u, "loongarch64 userspace prmd offset mismatch");
_Static_assert(sizeof(struct exception_frame) == 288u, "loongarch64 userspace frame size mismatch");

static bool loongarch64_frame_is_user(const struct exception_frame* frame) {
	return frame != NULL && (frame->prmd & LOONGARCH64_PRMD_PPLV_MASK) == LOONGARCH64_PRMD_PPLV_USER;
}

bool hal_userspace_thread_context_init(struct thread_context* context, uintptr_t kernel_stack_top, uintptr_t user_entry,
                                       uintptr_t user_stack, uintptr_t user_arg) {
	uintptr_t kernel_sp;
	uintptr_t user_sp;

	if (context == NULL || kernel_stack_top == 0u || user_entry == 0u || user_stack == 0u) return false;

	kernel_sp = kernel_stack_top & ~((uintptr_t)LOONGARCH64_USER_STACK_ALIGNMENT - 1u);
	user_sp   = user_stack & ~((uintptr_t)LOONGARCH64_USER_STACK_ALIGNMENT - 1u);
	if (kernel_sp == 0u || user_sp == 0u) return false;

	memset(context, 0, sizeof(*context));
	context->instruction_pointer              = (uintptr_t)loongarch64_userspace_enter;
	context->stack_pointer                    = kernel_sp;
	context->spill[LOONGARCH64_THREAD_CTX_S0] = user_entry;
	context->spill[LOONGARCH64_THREAD_CTX_S1] = user_arg;
	context->spill[LOONGARCH64_THREAD_CTX_S2] = user_sp;
	return true;
}

bool hal_userspace_frame_is_user(const struct hal_userspace_return_frame* frame) {
	return loongarch64_frame_is_user((const struct exception_frame*)frame);
}

bool hal_userspace_context_save(struct hal_userspace_context* context, const struct hal_userspace_return_frame* frame) {
	const struct exception_frame* native = (const struct exception_frame*)frame;
	struct arch_userspace_context saved;

	if (context == NULL || !loongarch64_frame_is_user(native)) return false;
	saved = (struct arch_userspace_context){
		.magic = HAL_USERSPACE_CONTEXT_MAGIC,
		.frame = *native,
	};
	memset(context, 0, sizeof(*context));
	memcpy(context->opaque, &saved, sizeof(saved));
	return true;
}

bool hal_userspace_context_restore(struct hal_userspace_return_frame*  frame,
                                   const struct hal_userspace_context* context) {
	struct exception_frame*       native = (struct exception_frame*)frame;
	struct arch_userspace_context saved;

	if (context == NULL || !loongarch64_frame_is_user(native)) return false;
	memcpy(&saved, context->opaque, sizeof(saved));
	if (saved.magic != HAL_USERSPACE_CONTEXT_MAGIC || !loongarch64_frame_is_user(&saved.frame)) return false;
	*native = saved.frame;
	return true;
}

bool hal_userspace_frame_redirect(struct hal_userspace_return_frame* frame, uintptr_t entry, uintptr_t stack_top,
                                  uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3) {
	struct exception_frame* native = (struct exception_frame*)frame;

	if (!loongarch64_frame_is_user(native) || entry == 0u || stack_top == 0u) return false;
	if ((entry & (LOONGARCH64_USER_INSTRUCTION_ALIGNMENT - 1u)) != 0u) return false;
	if ((stack_top & (HAL_USERSPACE_STACK_ALIGNMENT - 1u)) != 0u) return false;

	native->era                     = entry;
	native->gpr[LOONGARCH64_GPR_RA] = 0u;
	native->gpr[LOONGARCH64_GPR_SP] = stack_top;
	native->gpr[LOONGARCH64_GPR_A0] = arg0;
	native->gpr[LOONGARCH64_GPR_A1] = arg1;
	native->gpr[LOONGARCH64_GPR_A2] = arg2;
	native->gpr[LOONGARCH64_GPR_A3] = arg3;
	return true;
}
