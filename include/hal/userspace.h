#pragma once

#include <hal/cpu.h>
#include <stdbool.h>
#include <stdint.h>

/* Architecture hook for entry into userspace. */

#define HAL_USERSPACE_INTEGER_CONTEXT_SIZE 320u
#define HAL_USERSPACE_CONTEXT_ALIGNMENT 16u
#define HAL_USERSPACE_CONTEXT_SIZE                                                                                     \
	((HAL_USERSPACE_INTEGER_CONTEXT_SIZE + HAL_CPU_FP_CONTEXT_STORAGE_SIZE + HAL_USERSPACE_CONTEXT_ALIGNMENT - 1u) &   \
	 ~(HAL_USERSPACE_CONTEXT_ALIGNMENT - 1u))
#define HAL_USERSPACE_STACK_ALIGNMENT 16u

/* Native trap/syscall frame owned by the active architecture backend. */
struct hal_userspace_return_frame;

/* Complete userspace integer/control and floating-point/SIMD context. */
struct hal_userspace_context {
	_Alignas(HAL_USERSPACE_CONTEXT_ALIGNMENT) unsigned char opaque[HAL_USERSPACE_INTEGER_CONTEXT_SIZE];
	struct hal_cpu_fp_context fp_context;
};

_Static_assert((HAL_USERSPACE_CONTEXT_ALIGNMENT & (HAL_USERSPACE_CONTEXT_ALIGNMENT - 1u)) == 0u,
               "userspace context alignment must be a power of two");
_Static_assert((HAL_USERSPACE_STACK_ALIGNMENT & (HAL_USERSPACE_STACK_ALIGNMENT - 1u)) == 0u,
               "userspace stack alignment must be a power of two");
_Static_assert(sizeof(struct hal_userspace_context) == HAL_USERSPACE_CONTEXT_SIZE,
               "userspace context storage size mismatch");
_Static_assert(_Alignof(struct hal_userspace_context) == HAL_USERSPACE_CONTEXT_ALIGNMENT,
               "userspace context storage alignment mismatch");

/*
 * Initialize a thread frame so the first context switch into it
 * enters user_entry(user_arg) on the supplied user stack.
 */
bool hal_userspace_thread_context_init(struct thread_context* context, uintptr_t kernel_stack_top, uintptr_t user_entry,
                                       uintptr_t user_stack, uintptr_t user_arg);

/* Return true only when frame represents a final return to userspace. */
bool hal_userspace_frame_is_user(const struct hal_userspace_return_frame* frame);

/* Save or restore the complete integer/control and floating-point/SIMD userspace state. */
bool hal_userspace_context_save(struct hal_userspace_context* context, const struct hal_userspace_return_frame* frame);
bool hal_userspace_context_restore(struct hal_userspace_return_frame*  frame,
                                   const struct hal_userspace_context* context);

/* Redirect a userspace return frame to entry using the supplied top-of-stack. */
bool hal_userspace_frame_redirect(struct hal_userspace_return_frame* frame, uintptr_t entry, uintptr_t stack_top,
                                  uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3);
