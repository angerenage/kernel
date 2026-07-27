#include <hal/userspace.h>
#include <stddef.h>
#include <string.h>

struct hal_userspace_return_frame {
	bool      user;
	uintptr_t entry;
	uintptr_t stack;
	uintptr_t args[4];
};

#define MOCK_USERSPACE_CONTEXT_MAGIC 0x757063616c6c7631ull

struct mock_userspace_context {
	uint64_t                          magic;
	struct hal_userspace_return_frame frame;
};

static bool userspace_context_init_result = true;

_Static_assert(sizeof(struct mock_userspace_context) <= HAL_USERSPACE_CONTEXT_SIZE,
               "mock userspace context storage is too small");

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

bool hal_userspace_frame_is_user(const struct hal_userspace_return_frame* frame) {
	return frame != NULL && frame->user;
}

bool hal_userspace_context_save(struct hal_userspace_context* context, const struct hal_userspace_return_frame* frame) {
	struct mock_userspace_context saved;

	if (context == NULL || !hal_userspace_frame_is_user(frame)) return false;
	saved = (struct mock_userspace_context){
		.magic = MOCK_USERSPACE_CONTEXT_MAGIC,
		.frame = *frame,
	};
	memset(context, 0, sizeof(*context));
	memcpy(context->opaque, &saved, sizeof(saved));
	return true;
}

bool hal_userspace_context_restore(struct hal_userspace_return_frame*  frame,
                                   const struct hal_userspace_context* context) {
	struct mock_userspace_context saved;

	if (context == NULL || !hal_userspace_frame_is_user(frame)) return false;
	memcpy(&saved, context->opaque, sizeof(saved));
	if (saved.magic != MOCK_USERSPACE_CONTEXT_MAGIC || !hal_userspace_frame_is_user(&saved.frame)) return false;
	*frame = saved.frame;
	return true;
}

bool hal_userspace_frame_redirect(struct hal_userspace_return_frame* frame, uintptr_t entry, uintptr_t stack,
                                  uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3) {
	if (!hal_userspace_frame_is_user(frame) || entry == 0u || stack == 0u) return false;
	if ((stack & (HAL_USERSPACE_STACK_ALIGNMENT - 1u)) != 0u) return false;

	frame->entry   = entry;
	frame->stack   = stack;
	frame->args[0] = arg0;
	frame->args[1] = arg1;
	frame->args[2] = arg2;
	frame->args[3] = arg3;
	return true;
}

void hal_userspace_mock_set_context_init_result(bool result) {
	userspace_context_init_result = result;
}
