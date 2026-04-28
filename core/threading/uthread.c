#include <core/cpu.h>
#include <core/pmm.h>
#include <core/sched.h>
#include <core/uthread.h>
#include <core/vaddr_alloc.h>
#include <hal/userspace.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
	UTHREAD_KERNEL_STACK_PAGES = 4u,
};

static void uthread_release_stacks(struct uthread* thread) {
	if (thread == NULL) return;

	if (thread->user_stack_id != VMM_ID_INVALID && thread->address_space != NULL) {
		(void)vmm_free(thread->address_space, thread->user_stack_id);
		thread->user_stack_id = VMM_ID_INVALID;
	}
	if (thread->kernel_stack_id != VMM_ID_INVALID) {
		(void)vmm_free(address_space_kernel(), thread->kernel_stack_id);
		thread->kernel_stack_id = VMM_ID_INVALID;
	}
}

enum uthread_start_result uthread_start(struct uthread* thread, const struct uthread_start_params* params) {
	struct vmm_alloc_params user_stack_params = {
		.align_pages = 1u,
		.prot        = VMM_PROT_READ | VMM_PROT_WRITE | VMM_PROT_USER,
		.kind        = VMM_KIND_STACK,
		.guard_pages = VMM_STACK_DEFAULT_GUARD_PAGES,
		.map_flags   = 0u,
	};
	struct vmm_alloc_params kernel_stack_params = {
		.page_count  = UTHREAD_KERNEL_STACK_PAGES,
		.align_pages = 1u,
		.prot        = VMM_PROT_READ | VMM_PROT_WRITE | VMM_PROT_GLOBAL,
		.kind        = VMM_KIND_STACK,
		.guard_pages = VMM_STACK_DEFAULT_GUARD_PAGES,
		.map_flags   = 0u,
	};
	struct thread_context        context;
	struct thread_context_params thread_params;
	enum thread_init_result      init_result;
	void*                        user_stack_base   = NULL;
	void*                        kernel_stack_base = NULL;
	size_t                       user_stack_pages;
	uintptr_t                    kernel_stack_top;

	if (thread == NULL || params == NULL || params->address_space == NULL || params->user_entry == 0u) {
		return UTHREAD_START_INVALID_ARGUMENTS;
	}
	if (!address_space_is_initialized(params->address_space)) return UTHREAD_START_INVALID_ARGUMENTS;

	*thread = (struct uthread){
		.address_space   = params->address_space,
		.user_stack_id   = VMM_ID_INVALID,
		.kernel_stack_id = VMM_ID_INVALID,
	};

	user_stack_pages = params->user_stack_pages != 0u ? params->user_stack_pages : UTHREAD_DEFAULT_USER_STACK_PAGES;
	user_stack_params.page_count = user_stack_pages;
	if (!vmm_alloc(thread->address_space, &user_stack_params, &thread->user_stack_id, &user_stack_base)) {
		uthread_release_stacks(thread);
		return UTHREAD_START_STACK_ALLOC_FAILED;
	}
	if (!vmm_alloc(address_space_kernel(), &kernel_stack_params, &thread->kernel_stack_id, &kernel_stack_base)) {
		uthread_release_stacks(thread);
		return UTHREAD_START_STACK_ALLOC_FAILED;
	}

	thread->user_stack_top = (uintptr_t)user_stack_base + user_stack_pages * (uintptr_t)PMM_PAGE_SIZE;
	kernel_stack_top       = (uintptr_t)kernel_stack_base + UTHREAD_KERNEL_STACK_PAGES * (uintptr_t)PMM_PAGE_SIZE;

	if (!hal_userspace_thread_context_init(
			&context, kernel_stack_top, params->user_entry, thread->user_stack_top, params->user_arg)) {
		uthread_release_stacks(thread);
		return UTHREAD_START_CONTEXT_UNSUPPORTED;
	}

	thread_params = (struct thread_context_params){
		.name              = params->name,
		.kernel_stack_base = (uintptr_t)kernel_stack_base,
		.kernel_stack_top  = kernel_stack_top,
		.address_space     = thread->address_space,
		.preferred_cpu     = params->preferred_cpu,
		.base_priority     = THREAD_PRIORITY_DEFAULT,
		.detached          = params->detached,
		.context           = context,
		.entry             = NULL,
		.arg               = NULL,
	};
	init_result = thread_init_context(&thread->thread, &thread_params);
	if (init_result != THREAD_INIT_OK) {
		uthread_release_stacks(thread);
		return UTHREAD_START_INVALID_ARGUMENTS;
	}
	if (!sched_make_runnable(&thread->thread)) {
		uthread_release_stacks(thread);
		return UTHREAD_START_SCHEDULER_REJECTED;
	}

	return UTHREAD_START_OK;
}

bool uthread_deinit(struct uthread* thread) {
	if (thread == NULL) return false;
	if (!thread_is_terminated(&thread->thread) && thread->thread.state != THREAD_STATE_NEW) return false;
	if (!thread_is_joinable(&thread->thread)) return false;

	uthread_release_stacks(thread);
	memset(thread, 0, sizeof(*thread));
	thread->user_stack_id   = VMM_ID_INVALID;
	thread->kernel_stack_id = VMM_ID_INVALID;
	return true;
}
