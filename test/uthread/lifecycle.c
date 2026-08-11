#include "test_support.h"

Test(uthread, deinit_detaches_joinable_thread_from_process) {
	struct process* process = NULL;
	struct uthread  worker  = {
		  .user_stack_id   = VMM_ID_INVALID,
		  .kernel_stack_id = VMM_ID_INVALID,
    };
	uthread_id_t                worker_tid;
	struct uthread_start_params params = {
		.name             = "user/joinable",
		.process          = NULL,
		.user_entry       = 0x400000u,
		.user_stack_pages = 2u,
		.preferred_cpu    = NULL,
		.detached         = false,
	};

	init_uthread_test_environment();
	process        = spawn_owner_process("test/process");
	params.process = process;

	cr_assert_eq(uthread_start(&worker, &params), UTHREAD_START_OK, "uthread_start failed");
	worker_tid = uthread_id(&worker);
	cr_assert_eq(process_thread_count(process), 2u, "started uthread should attach to process");

	thread_mark_zombie(&worker.thread);
	cr_assert(uthread_deinit(&worker), "uthread_deinit should reclaim terminated joinable user thread");
	cr_assert_eq(process_thread_count(process), 1u, "uthread_deinit should detach from process");
	cr_assert_null(uthread_lookup(worker_tid), "uthread_deinit should remove the TID registry entry");
	terminate_main_thread(process);
	cr_assert(process_destroy(process), "process_destroy should succeed after uthread_deinit");
}

Test(uthread, retained_descriptor_defers_final_cleanup) {
	struct process* process = NULL;
	struct uthread  worker  = {
		  .user_stack_id   = VMM_ID_INVALID,
		  .kernel_stack_id = VMM_ID_INVALID,
    };
	struct uthread* held;
	uthread_id_t    worker_tid;

	init_uthread_test_environment();
	process = spawn_owner_process("test/retained-descriptor");

	cr_assert_eq(uthread_start(&worker,
	                           &(const struct uthread_start_params){
								   .name             = "user/retained",
								   .process          = process,
								   .user_entry       = 0x400000u,
								   .user_stack_pages = 2u,
								   .detached         = false,
							   }),
	             UTHREAD_START_OK);

	worker_tid = uthread_id(&worker);
	held       = uthread_acquire(worker_tid);
	cr_assert_eq(held, &worker, "uthread_acquire should retain the registered descriptor");

	thread_mark_zombie(&worker.thread);
	cr_assert(uthread_deinit(&worker), "uthread_deinit should release the owner reference");
	cr_assert_null(uthread_lookup(worker_tid), "destroying a uthread must remove its ID immediately");
	cr_assert_null(uthread_acquire(worker_tid), "removed IDs must reject new retained lookups");
	cr_assert_eq(process_thread_count(process),
	             2u,
	             "an outstanding reference should defer process detachment and stack cleanup");

	uthread_release(held);
	cr_assert_eq(process_thread_count(process), 1u, "the last reference should perform final cleanup");
	cr_assert_eq(worker.user_stack_id, VMM_ID_INVALID);
	cr_assert_eq(worker.upcall.stack_id, VMM_ID_INVALID);
	cr_assert_eq(worker.kernel_stack_id, VMM_ID_INVALID);

	terminate_main_thread(process);
	cr_assert(process_destroy(process));
}
