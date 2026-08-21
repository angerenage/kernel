#include "test_support.h"

Test(uthread, context_initialization_failure_rolls_back_all_allocated_resources) {
	struct process* process = NULL;
	struct uthread  worker  = {
		  .user_stack_id   = VMM_ID_INVALID,
		  .kernel_stack_id = VMM_ID_INVALID,
    };
	size_t                    user_regions_before;
	size_t                    kernel_regions_before;
	size_t                    process_threads_before;
	size_t                    uthreads_before;
	size_t                    run_queue_before;
	enum uthread_start_result result;

	init_uthread_test_environment();
	process = spawn_owner_process("test/context-rollback");

	user_regions_before    = vm_space_mapping_count(process_address_space(process));
	kernel_regions_before  = vm_space_mapping_count(vm_space_kernel());
	process_threads_before = process_thread_count(process);
	uthreads_before        = uthread_count();
	run_queue_before       = sched_run_queue_depth(cpu_current());

	hal_userspace_mock_set_context_init_result(false);
	result = uthread_start(&worker,
	                       &(const struct uthread_start_params){
							   .name             = "user/context-failure",
							   .process          = process,
							   .user_entry       = 0x420000u,
							   .user_stack_pages = 2u,
							   .detached         = false,
						   });
	hal_userspace_mock_set_context_init_result(true);

	cr_assert_eq(result, UTHREAD_START_CONTEXT_UNSUPPORTED, "injected userspace context failure was not surfaced");
	cr_assert_eq(uthread_count(), uthreads_before, "failed start leaked a TID registration");
	cr_assert_eq(
		process_thread_count(process), process_threads_before, "failed start attached a thread to the process");
	cr_assert_eq(vm_space_mapping_count(process_address_space(process)),
	             user_regions_before,
	             "failed start leaked user/upcall stack regions");
	cr_assert_eq(vm_space_mapping_count(vm_space_kernel()),
	             kernel_regions_before,
	             "failed start leaked its kernel stack region");
	cr_assert_eq(
		sched_run_queue_depth(cpu_current()), run_queue_before, "failed start changed scheduler run-queue membership");
	cr_assert_eq(worker.user_stack_id, VMM_ID_INVALID, "failed start retained a user-stack id");
	cr_assert_eq(worker.upcall.stack_id, VMM_ID_INVALID, "failed start retained an upcall-stack id");
	cr_assert_eq(worker.kernel_stack_id, VMM_ID_INVALID, "failed start retained a kernel-stack id");

	terminate_main_thread(process);
	cr_assert(process_destroy(process), "process_destroy failed after uthread rollback test");
}
