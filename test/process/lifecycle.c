#include "test_support.h"

Test(process, destroy_rejects_live_main_thread) {
	struct process* process = NULL;

	init_process_test_environment();

	cr_assert_eq(create_process_with_main_thread(&process,
	                                             &(const struct process_spawn_params){
													 .name       = "live",
													 .user_entry = 0x400000u,
												 }),
	             PROCESS_OK);
	cr_assert(!process_destroy(process), "process_destroy must reject live process threads");

	thread_mark_zombie(&process->main_thread->thread);
	cr_assert(process_destroy(process), "process_destroy should succeed after main thread exits");
}

Test(process, terminate_marks_process_exiting_and_requests_thread_cancellation) {
	struct process* process = NULL;

	init_process_test_environment();

	cr_assert_eq(create_process_with_main_thread(&process,
	                                             &(const struct process_spawn_params){
													 .name       = "terminating",
													 .user_entry = 0x400000u,
												 }),
	             PROCESS_OK);

	cr_assert(process_terminate(process, PROCESS_EXIT_MEMORY_PROTECTION),
	          "process_terminate should accept a running process");
	cr_assert_eq(process_get_state(process), PROCESS_STATE_EXITING, "process should enter EXITING state");
	cr_assert_eq(
		process->exit_code, PROCESS_EXIT_MEMORY_PROTECTION, "process_terminate should publish the process exit code");
	cr_assert(thread_cancel_requested(&process->main_thread->thread), "process threads should receive cancellation");

	thread_mark_zombie(&process->main_thread->thread);
	process_notify_thread_exit(process, &process->main_thread->thread, PROCESS_EXIT_MEMORY_PROTECTION);
	cr_assert_eq(process_get_state(process), PROCESS_STATE_ZOMBIE, "last thread exit should make process zombie");
	cr_assert(process_destroy(process), "process_destroy should reclaim the terminated process");
}

Test(process, join_returns_zombie_exit_code_once) {
	struct process* process   = NULL;
	uintptr_t       exit_code = 0u;

	init_process_test_environment();

	cr_assert_eq(create_process_with_main_thread(&process,
	                                             &(const struct process_spawn_params){
													 .name       = "joinable",
													 .user_entry = 0x400000u,
												 }),
	             PROCESS_OK);

	cr_assert(process_terminate(process, 123u));
	thread_mark_zombie(&process->main_thread->thread);
	process_notify_thread_exit(process, &process->main_thread->thread, 123u);

	cr_assert_eq(process_join(process, &exit_code), PROCESS_JOIN_OK, "join should accept a zombie process");
	cr_assert_eq(exit_code, 123u, "join should publish the process exit code");
	cr_assert_eq(process_join(process, NULL), PROCESS_JOIN_ALREADY_JOINED, "process should be joined only once");

	cr_assert(process_destroy(process), "process_destroy should reclaim a joined zombie process");
}

Test(process, detach_prevents_later_join) {
	struct process* process = NULL;

	init_process_test_environment();

	cr_assert_eq(create_process_with_main_thread(&process,
	                                             &(const struct process_spawn_params){
													 .name       = "detached",
													 .user_entry = 0x400000u,
												 }),
	             PROCESS_OK);

	cr_assert_eq(process_detach(process), PROCESS_DETACH_OK, "process_detach should accept a joinable process");
	cr_assert_eq(process_detach(process), PROCESS_DETACH_ALREADY_DETACHED, "process_detach should reject repeats");
	cr_assert_eq(process_join(process, NULL), PROCESS_JOIN_DETACHED, "detached process should not be joinable");

	thread_mark_zombie(&process->main_thread->thread);
	process_notify_thread_exit(process, &process->main_thread->thread, 0u);
	cr_assert(process_destroy(process), "process_destroy should reclaim detached zombie process");
}

Test(process, destroy_reclaims_message_queue_heap_storage) {
	struct process* warmup  = NULL;
	struct process* process = NULL;
	size_t          baseline;

	init_process_test_environment();

	cr_assert_eq(process_create(&warmup, NULL), PROCESS_OK, "warmup process_create failed");
	cr_assert_not_null(warmup->message_queue.data, "warmup process did not initialize its message queue");
	cr_assert(process_destroy(warmup), "warmup process_destroy failed");

	baseline = heap_free_bytes();

	cr_assert_eq(process_create(&process, NULL), PROCESS_OK, "process_create failed");
	cr_assert_not_null(process->message_queue.data, "created process did not initialize its message queue");
	cr_assert_lt(heap_free_bytes(), baseline, "process creation should consume heap storage");
	cr_assert(process_destroy(process), "process_destroy failed");
	cr_assert_eq(heap_free_bytes(), baseline, "destroying a process must release its message queue backing storage");
}

Test(process, acquired_reference_defers_final_teardown) {
	struct process* warmup   = NULL;
	struct process* process  = NULL;
	struct process* retained = NULL;
	process_id_t    pid;
	size_t          baseline;
	uint8_t         payload = 0x5au;

	init_process_test_environment();
	cr_assert_eq(process_create(&warmup, NULL), PROCESS_OK);
	cr_assert(process_destroy(warmup));
	baseline = heap_free_bytes();

	cr_assert_eq(process_create(&process, NULL), PROCESS_OK);
	pid      = process_pid(process);
	retained = process_acquire(pid);
	cr_assert_eq(retained, process);

	cr_assert(process_destroy(process));
	cr_assert_null(process_lookup(pid), "destroy must prevent new acquisitions before teardown");
	cr_assert_not_null(retained->message_queue.data, "an acquired process must retain its message queue");
	cr_assert_eq(message_queue_send(&retained->message_queue, 1u, &payload, sizeof(payload)), MESSAGE_OK);
	cr_assert_lt(heap_free_bytes(), baseline, "retained process storage must remain allocated");

	process_release(retained);
	cr_assert_eq(heap_free_bytes(), baseline, "the last process reference must complete teardown");
}
