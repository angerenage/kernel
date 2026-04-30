#include <hal/userspace.h>
#include <stddef.h>

static bool userspace_context_init_result = true;

bool hal_userspace_thread_context_init(struct thread_context* context, uintptr_t kernel_stack_top, uintptr_t user_entry,
                                       uintptr_t user_stack, uintptr_t user_arg) {
	if (!userspace_context_init_result) return false;
	if (context == NULL || kernel_stack_top == 0u || user_entry == 0u || user_stack == 0u) return false;

	*context = (struct thread_context){
		.instruction_pointer = user_entry,
		.stack_pointer       = kernel_stack_top,
	};
	context->spill[0] = user_stack;
	context->spill[1] = user_arg;
	return true;
}

void hal_userspace_mock_set_context_init_result(bool result) {
	userspace_context_init_result = result;
}
