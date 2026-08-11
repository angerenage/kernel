#include "test_support.h"

Test(uthread, detached_start_registers_finalizer_before_queueing) {
	struct process* process = NULL;
	struct uthread  worker  = {
		  .user_stack_id   = VMM_ID_INVALID,
		  .kernel_stack_id = VMM_ID_INVALID,
    };
	enum uthread_start_result   result;
	struct uthread_start_params params = {
		.name             = "user/detached",
		.process          = NULL,
		.user_entry       = 0x400000u,
		.user_stack_pages = 2u,
		.preferred_cpu    = NULL,
		.detached         = true,
	};

	init_uthread_test_environment();
	process        = spawn_owner_process("test/process");
	params.process = process;

	result = uthread_start(&worker, &params);
	cr_assert_eq(result, UTHREAD_START_OK, "uthread_start failed: %d", result);

	cr_assert_neq(worker.upcall.stack_id, VMM_ID_INVALID, "uthread_start should allocate an upcall stack");
	cr_assert_neq(worker.upcall.stack_top, 0u, "uthread_start should publish the upcall stack top");
	cr_assert_eq(
		worker.upcall.stack_top & (HAL_USERSPACE_STACK_ALIGNMENT - 1u), 0u, "upcall stack top should be aligned");
	cr_assert_eq(worker.process, process, "uthread should retain its owning process");
	cr_assert_neq(uthread_id(&worker), UTHREAD_ID_INVALID, "uthread_start should assign a valid TID");
	cr_assert_eq(uthread_lookup(uthread_id(&worker)), &worker, "TID lookup should return the started uthread");
	cr_assert_eq(worker.thread.address_space,
	             process_address_space(process),
	             "scheduler thread should use the process address space");
	cr_assert_eq(process_thread_count(process), 2u, "started uthread should attach to its process");
	cr_assert_eq(
		process_get_state(process), PROCESS_STATE_RUNNING, "process should become running after thread attach");
	cr_assert(!process_destroy(process), "process_destroy must reject a process with live threads");
	cr_assert(!thread_is_joinable(&worker.thread), "detached user thread must not be joinable");
	cr_assert_not_null(worker.thread.reap_callback, "detached user thread must have a finalizer callback");
	cr_assert_eq(worker.thread.reap_context, &worker, "finalizer callback should receive the uthread wrapper");
	cr_assert_eq(worker.heap_allocated, false, "caller-owned uthread_start must not mark storage heap-owned");
	cr_assert_eq(sched_run_queue_depth(cpu_current()), 2u, "only the process threads should be runnable");
}
