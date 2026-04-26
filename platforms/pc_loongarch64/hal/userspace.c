#include <hal/userspace.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

enum {
	LOONGARCH64_USER_STACK_ALIGNMENT = 16u,
	LOONGARCH64_THREAD_CTX_S0        = 1,
	LOONGARCH64_THREAD_CTX_S1,
	LOONGARCH64_THREAD_CTX_S2,
};

extern void loongarch64_userspace_enter(void);

_Static_assert(HAL_CPU_THREAD_CONTEXT_SPILL_WORDS > LOONGARCH64_THREAD_CTX_S2,
               "loongarch64 userspace spill area is too small");

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
