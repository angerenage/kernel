#include "test_support.h"

Test(process, spawn_thread_allocates_joinable_thread_in_process_address_space) {
	struct process* process = NULL;
	struct uthread* worker  = NULL;
	uthread_id_t    worker_tid;

	init_process_test_environment();

	cr_assert_eq(create_process_with_main_thread(&process,
	                                             &(const struct process_spawn_params){
													 .name       = "owner",
													 .user_entry = 0x400000u,
												 }),
	             PROCESS_OK);

	cr_assert_eq(process_spawn_thread(process,
	                                  &worker,
	                                  &(const struct process_thread_params){
										  .name             = "spawned-worker",
										  .user_entry       = 0x410000u,
										  .user_stack_pages = 2u,
										  .preferred_cpu    = NULL,
										  .detached         = false,
									  }),
	             PROCESS_THREAD_SPAWN_OK);
	cr_assert_not_null(worker, "process_spawn_thread should return the allocated thread");
	cr_assert(worker->heap_allocated, "process_spawn_thread should mark the thread descriptor heap-owned");
	cr_assert_eq(worker->process, process, "spawned thread should retain its owning process");
	worker_tid = uthread_id(worker);
	cr_assert_neq(worker_tid, UTHREAD_ID_INVALID, "spawned thread should receive a valid TID");
	cr_assert_eq(uthread_lookup(worker_tid), worker, "spawned thread TID lookup should return the thread");
	cr_assert_eq(worker->thread.address_space,
	             process_address_space(process),
	             "spawned thread should use the process address space");
	cr_assert_eq(process_thread_count(process), 2u, "process should track the main thread and spawned thread");
	cr_assert_eq(sched_run_queue_depth(cpu_current()), 2u, "main and spawned threads should be runnable");

	terminate_process_thread(worker);
	terminate_main_thread(process);
	cr_assert(process_destroy(process), "process_destroy should reclaim heap-allocated process threads");
	cr_assert_null(uthread_lookup(worker_tid), "destroyed spawned thread should not remain registered");
	cr_assert_eq(uthread_count(), 0u, "thread registry should be empty after process destroy");
}

Test(process, spawn_thread_rejects_missing_arguments) {
	struct process* process = NULL;
	struct uthread* worker  = NULL;

	init_process_test_environment();

	cr_assert_eq(create_process_with_main_thread(&process,
	                                             &(const struct process_spawn_params){
													 .name       = "owner",
													 .user_entry = 0x400000u,
												 }),
	             PROCESS_OK);

	cr_assert_eq(process_spawn_thread(NULL,
	                                  &worker,
	                                  &(const struct process_thread_params){
										  .name       = "worker",
										  .user_entry = 0x410000u,
									  }),
	             PROCESS_THREAD_SPAWN_INVALID_ARGUMENTS);
	cr_assert_null(worker, "failed process_spawn_thread should not publish a thread");
	cr_assert_eq(process_spawn_thread(process, &worker, NULL), PROCESS_THREAD_SPAWN_INVALID_ARGUMENTS);
	cr_assert_null(worker, "failed process_spawn_thread should leave output NULL");
	cr_assert_eq(process_spawn_thread(process,
	                                  NULL,
	                                  &(const struct process_thread_params){
										  .name       = "worker",
										  .user_entry = 0x410000u,
										  .detached   = false,
									  }),
	             PROCESS_THREAD_SPAWN_INVALID_ARGUMENTS);

	terminate_main_thread(process);
	cr_assert(process_destroy(process), "process_destroy should reclaim terminated main thread");
}

Test(process, join_thread_returns_exit_code_and_reclaims_thread) {
	struct process* process   = NULL;
	struct uthread* worker    = NULL;
	uintptr_t       exit_code = 0u;
	uthread_id_t    worker_tid;

	init_process_test_environment();

	cr_assert_eq(create_process_with_main_thread(&process,
	                                             &(const struct process_spawn_params){
													 .name       = "owner",
													 .user_entry = 0x400000u,
												 }),
	             PROCESS_OK);
	cr_assert_eq(process_spawn_thread(process,
	                                  &worker,
	                                  &(const struct process_thread_params){
										  .name       = "worker",
										  .user_entry = 0x410000u,
									  }),
	             PROCESS_THREAD_SPAWN_OK);
	worker_tid = uthread_id(worker);

	thread_mark_exiting(&worker->thread, 44u);
	thread_mark_zombie(&worker->thread);
	cr_assert_eq(process_join_thread(process, worker, &exit_code),
	             PROCESS_THREAD_JOIN_OK,
	             "process_join_thread should join a terminated process thread");
	cr_assert_eq(exit_code, 44u, "process_join_thread should publish thread exit code");
	cr_assert_eq(process_thread_count(process), 1u, "joined thread should detach from process");
	cr_assert_null(uthread_lookup(worker_tid), "joined thread should be removed from TID registry");

	terminate_main_thread(process);
	cr_assert(process_destroy(process), "process_destroy should reclaim remaining main thread");
}

Test(process, cancel_thread_requests_deferred_cancellation) {
	struct process* process = NULL;
	struct uthread* worker  = NULL;

	init_process_test_environment();

	cr_assert_eq(create_process_with_main_thread(&process,
	                                             &(const struct process_spawn_params){
													 .name       = "owner",
													 .user_entry = 0x400000u,
												 }),
	             PROCESS_OK);
	cr_assert_eq(process_spawn_thread(process,
	                                  &worker,
	                                  &(const struct process_thread_params){
										  .name       = "worker",
										  .user_entry = 0x410000u,
									  }),
	             PROCESS_THREAD_SPAWN_OK);

	cr_assert_eq(process_cancel_thread(process, worker),
	             PROCESS_THREAD_CANCEL_OK,
	             "process_cancel_thread should accept a live process thread");
	cr_assert(thread_cancel_requested(&worker->thread), "target thread should record cancellation request");

	terminate_process_thread(worker);
	terminate_main_thread(process);
	cr_assert(process_destroy(process), "process_destroy should reclaim canceled terminated thread");
}

Test(process, detach_thread_marks_thread_unjoinable) {
	struct process* process = NULL;
	struct uthread* worker  = NULL;

	init_process_test_environment();

	cr_assert_eq(create_process_with_main_thread(&process,
	                                             &(const struct process_spawn_params){
													 .name       = "owner",
													 .user_entry = 0x400000u,
												 }),
	             PROCESS_OK);
	cr_assert_eq(process_spawn_thread(process,
	                                  &worker,
	                                  &(const struct process_thread_params){
										  .name       = "worker",
										  .user_entry = 0x410000u,
									  }),
	             PROCESS_THREAD_SPAWN_OK);

	cr_assert_eq(process_detach_thread(process, worker),
	             PROCESS_THREAD_DETACH_OK,
	             "process_detach_thread should accept a live joinable process thread");
	cr_assert_eq(process_join_thread(process, worker, NULL),
	             PROCESS_THREAD_JOIN_DETACHED,
	             "detached process thread should reject join");
}

Test(process, detached_spawn_does_not_publish_a_thread_handle) {
	struct process* process   = NULL;
	struct uthread* published = (struct uthread*)(uintptr_t)0x1u;
	struct uthread* detached;

	init_process_test_environment();
	cr_assert_eq(create_process_with_main_thread(&process,
	                                             &(const struct process_spawn_params){
													 .name       = "detached-owner",
													 .user_entry = 0x400000u,
												 }),
	             PROCESS_OK);

	cr_assert_eq(process_spawn_thread(process,
	                                  &published,
	                                  &(const struct process_thread_params){
										  .name       = "detached-worker",
										  .user_entry = 0x410000u,
										  .detached   = true,
									  }),
	             PROCESS_THREAD_SPAWN_OK,
	             "detached process thread spawn failed");
	cr_assert_null(published, "process_spawn_thread documents detached threads as not publishing a handle");

	detached = process->thread_tail;
	cr_assert_not_null(detached, "detached thread must still be attached to its process internally");
	cr_assert_neq(detached, process->main_thread, "detached worker was not added after the main thread");
	cr_assert_not(thread_is_joinable(&detached->thread), "spawned detached worker must be unjoinable");

	thread_mark_zombie(&detached->thread);
	thread_notify_reap(&detached->thread);
	terminate_main_thread(process);
	cr_assert(process_destroy(process), "process_destroy failed after detached worker reap");
}

Test(process, prepared_main_thread_is_not_visible_or_runnable_until_commit) {
	struct process* process = NULL;
	struct uthread* thread  = NULL;
	uthread_id_t    tid;
	size_t          run_queue_before;

	init_process_test_environment();
	cr_assert_eq(process_create(&process, "prepared-main"), PROCESS_OK, "process_create failed");
	run_queue_before = sched_run_queue_depth(cpu_current());

	cr_assert_eq(process_prepare_main_thread(process,
	                                         &thread,
	                                         &(const struct process_thread_params){
												 .name       = "prepared-main",
												 .user_entry = 0x400000u,
											 }),
	             PROCESS_THREAD_SPAWN_OK,
	             "main-thread preparation failed");
	cr_assert_not_null(thread, "preparation must publish its private thread descriptor");
	tid = uthread_id(thread);
	cr_assert_neq(tid, UTHREAD_ID_INVALID, "prepared thread should already own a stable TID");
	cr_assert_eq(uthread_lookup(tid), thread, "prepared thread should be registered for capability creation");
	cr_assert_eq(thread->thread.state, THREAD_STATE_NEW, "prepared thread must remain NEW");
	cr_assert_eq(process_get_state(process), PROCESS_STATE_NEW, "preparation must not start the process");
	cr_assert_null(process_main_thread(process), "preparation must not publish the process main thread");
	cr_assert_eq(process_thread_count(process), 0u, "preparation must not attach the thread to the process");
	cr_assert_eq(
		sched_run_queue_depth(cpu_current()), run_queue_before, "preparation must not make the thread runnable");

	cr_assert(process_abort_main_thread(process, thread), "prepared main-thread abort failed");
	cr_assert_null(uthread_lookup(tid), "aborted prepared thread should be removed from the TID registry");
	cr_assert_eq(process_get_state(process), PROCESS_STATE_NEW, "abort must leave the process NEW");
	cr_assert_eq(process_thread_count(process), 0u, "abort must leave the process without threads");

	cr_assert_eq(process_start_main_thread(process,
	                                       &thread,
	                                       &(const struct process_thread_params){
											   .name       = "prepared-main/retry",
											   .user_entry = 0x410000u,
										   }),
	             PROCESS_THREAD_SPAWN_OK,
	             "aborting a prepared main thread should release the unique-main-thread claim");
	terminate_main_thread(process);
	cr_assert(process_destroy(process), "process_destroy failed after prepared-main retry");
}

Test(process, commit_main_thread_publishes_and_schedules_exactly_once) {
	struct process* process = NULL;
	struct uthread* thread  = NULL;
	size_t          run_queue_before;

	init_process_test_environment();
	cr_assert_eq(process_create(&process, "commit-main"), PROCESS_OK, "process_create failed");
	run_queue_before = sched_run_queue_depth(cpu_current());
	cr_assert_eq(process_prepare_main_thread(process,
	                                         &thread,
	                                         &(const struct process_thread_params){
												 .name       = "commit-main",
												 .user_entry = 0x400000u,
											 }),
	             PROCESS_THREAD_SPAWN_OK);

	cr_assert_eq(process_commit_main_thread(process, thread), PROCESS_THREAD_SPAWN_OK, "main-thread commit failed");
	cr_assert_eq(process_get_state(process), PROCESS_STATE_RUNNING, "commit should start the process lifecycle");
	cr_assert_eq(process_main_thread(process), thread, "commit should publish the prepared thread as main_thread");
	cr_assert_eq(process_thread_count(process), 1u, "commit should attach exactly one thread");
	cr_assert_eq(sched_run_queue_depth(cpu_current()),
	             run_queue_before + 1u,
	             "commit should make exactly the prepared thread runnable");
	cr_assert_eq(process_commit_main_thread(process, thread),
	             PROCESS_THREAD_SPAWN_INVALID_ARGUMENTS,
	             "a committed main thread must not be committed twice");
	cr_assert_not(process_abort_main_thread(process, thread), "a committed main thread must no longer be abortable");

	terminate_main_thread(process);
	cr_assert(process_destroy(process), "process_destroy failed after main-thread commit test");
}

Test(process, prepared_worker_is_not_attached_or_runnable_until_commit) {
	struct process* process = NULL;
	struct uthread* worker  = NULL;
	uthread_id_t    first_tid;
	size_t          threads_before;
	size_t          run_queue_before;

	init_process_test_environment();
	cr_assert_eq(create_process_with_main_thread(&process,
	                                             &(const struct process_spawn_params){
													 .name       = "prepared-worker-owner",
													 .user_entry = 0x400000u,
												 }),
	             PROCESS_OK);
	threads_before   = process_thread_count(process);
	run_queue_before = sched_run_queue_depth(cpu_current());

	cr_assert_eq(process_prepare_thread(process,
	                                    &worker,
	                                    &(const struct process_thread_params){
											.name       = "prepared-worker",
											.user_entry = 0x410000u,
										}),
	             PROCESS_THREAD_SPAWN_OK,
	             "worker preparation failed");
	cr_assert_not_null(worker);
	first_tid = uthread_id(worker);
	cr_assert_neq(first_tid, UTHREAD_ID_INVALID, "prepared worker should have a stable TID");
	cr_assert_eq(worker->thread.state, THREAD_STATE_NEW, "prepared worker must remain NEW");
	cr_assert_eq(process_thread_count(process), threads_before, "prepared worker must not be attached");
	cr_assert_eq(sched_run_queue_depth(cpu_current()), run_queue_before, "prepared worker must not be runnable");

	cr_assert(process_abort_thread(process, worker), "prepared worker abort failed");
	cr_assert_null(uthread_lookup(first_tid), "aborted worker should be removed from the TID registry");
	cr_assert_eq(process_thread_count(process), threads_before, "worker abort changed process thread count");
	cr_assert_eq(sched_run_queue_depth(cpu_current()), run_queue_before, "worker abort changed run queue depth");

	worker = NULL;
	cr_assert_eq(process_prepare_thread(process,
	                                    &worker,
	                                    &(const struct process_thread_params){
											.name       = "prepared-worker/retry",
											.user_entry = 0x420000u,
										}),
	             PROCESS_THREAD_SPAWN_OK,
	             "worker retry preparation failed");
	cr_assert_eq(process_commit_thread(process, worker), PROCESS_THREAD_SPAWN_OK, "worker commit failed");
	cr_assert_eq(process_thread_count(process), threads_before + 1u, "worker commit should attach one thread");
	cr_assert_eq(sched_run_queue_depth(cpu_current()), run_queue_before + 1u, "worker commit should queue one thread");
	cr_assert_eq(process_commit_thread(process, worker),
	             PROCESS_THREAD_SPAWN_INVALID_ARGUMENTS,
	             "committed worker must not be committed twice");
	cr_assert_not(process_abort_thread(process, worker), "committed worker must no longer be abortable");

	terminate_process_thread(worker);
	terminate_main_thread(process);
	cr_assert(process_destroy(process), "process_destroy failed after prepared-worker test");
}
