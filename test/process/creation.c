#include "test_support.h"

Test(process, create_initializes_pid_new_state_and_address_space) {
	struct process*       process = NULL;
	struct address_space* space;
	enum process_result   result;
	process_id_t          pid;

	init_process_test_environment();

	result = process_create(&process, "init");
	cr_assert_eq(result, PROCESS_OK, "process_create failed: %d", result);
	cr_assert_not_null(process, "process_create did not return a process");
	pid = process_pid(process);
	cr_assert_neq(pid, PROCESS_PID_INVALID, "process pid should be valid");
	cr_assert_eq(process_lookup(pid), process, "process PID lookup should return the process");
	cr_assert_eq(process_count(), 1u, "process registry should track the created process");
	cr_assert_eq(process_get_state(process), PROCESS_STATE_NEW, "created process should start new");
	cr_assert_eq(process_thread_count(process), 0u, "created process should not have threads yet");
	cr_assert_null(process_main_thread(process), "created process should not have a main thread yet");

	space = process_address_space(process);
	cr_assert_not_null(space, "process address space should be exposed");
	cr_assert(address_space_is_initialized(space), "process address space should be initialized");
	cr_assert_eq(address_space_total_page_count(space), MM_USER_VMM_SIZE / PMM_PAGE_SIZE);
	cr_assert_eq(address_space_free_page_count(space), MM_USER_VMM_SIZE / PMM_PAGE_SIZE);

	cr_assert(process_destroy(process), "process_destroy failed");
	cr_assert_null(process_lookup(pid), "destroyed process should not remain registered");
	cr_assert_eq(process_count(), 0u, "process registry should be empty after destroy");
}

Test(process, spawn_thread_sets_running_state_and_main_thread) {
	struct process*       process = NULL;
	struct address_space* space;
	enum process_result   result;
	process_id_t          pid;
	uthread_id_t          main_tid;

	init_process_test_environment();

	result = create_process_with_main_thread(&process,
	                                         &(const struct process_spawn_params){
												 .name       = "init",
												 .user_entry = 0x400000u,
											 });
	cr_assert_eq(result, PROCESS_OK, "process_create/process_spawn_thread failed: %d", result);
	cr_assert_not_null(process, "process_create did not return a process");
	pid = process_pid(process);
	cr_assert_neq(pid, PROCESS_PID_INVALID, "process pid should be valid");
	cr_assert_eq(process_lookup(pid), process, "process PID lookup should return the process");
	cr_assert_eq(process_count(), 1u, "process registry should track the process");
	cr_assert_eq(process_get_state(process), PROCESS_STATE_RUNNING, "process should run after its main thread starts");
	cr_assert_eq(process_thread_count(process), 1u, "process should have one main thread");
	cr_assert_not_null(process->main_thread, "process should record its main thread");
	main_tid = uthread_id(process->main_thread);
	cr_assert_neq(main_tid, UTHREAD_ID_INVALID, "main thread TID should be valid");
	cr_assert_eq(
		uthread_lookup(main_tid), process->main_thread, "main thread TID lookup should return the main thread");
	cr_assert_eq(
		process_main_thread(process), process->main_thread, "main thread accessor should return the main thread");

	space = process_address_space(process);
	cr_assert_not_null(space, "process address space should be exposed");
	cr_assert(address_space_is_initialized(space), "process address space should be initialized");
	cr_assert_eq(address_space_total_page_count(space), MM_USER_VMM_SIZE / PMM_PAGE_SIZE);
	cr_assert(address_space_free_page_count(space) < MM_USER_VMM_SIZE / PMM_PAGE_SIZE);

	terminate_main_thread(process);
	cr_assert(process_destroy(process), "process_destroy failed");
	cr_assert_null(process_lookup(pid), "destroyed process should not remain registered");
	cr_assert_eq(process_count(), 0u, "process registry should be empty after destroy");
	cr_assert_null(uthread_lookup(main_tid), "destroyed main thread should not remain registered");
}

Test(process, spawn_assigns_monotonic_pids) {
	struct process* first  = NULL;
	struct process* second = NULL;
	process_id_t    first_pid;
	process_id_t    second_pid;

	init_process_test_environment();

	cr_assert_eq(create_process_with_main_thread(&first,
	                                             &(const struct process_spawn_params){
													 .name       = "first",
													 .user_entry = 0x400000u,
												 }),
	             PROCESS_OK);
	cr_assert_eq(create_process_with_main_thread(&second,
	                                             &(const struct process_spawn_params){
													 .name       = "second",
													 .user_entry = 0x410000u,
												 }),
	             PROCESS_OK);

	first_pid  = process_pid(first);
	second_pid = process_pid(second);
	cr_assert_neq(first_pid, PROCESS_PID_INVALID);
	cr_assert_neq(second_pid, PROCESS_PID_INVALID);
	cr_assert(second_pid > first_pid, "second pid should be greater than first pid");

	terminate_main_thread(first);
	terminate_main_thread(second);
	cr_assert(process_destroy(first), "failed to destroy first process");
	cr_assert(process_destroy(second), "failed to destroy second process");
}

Test(process, spawn_rejects_missing_output_pointer) {
	init_process_test_environment();

	cr_assert_eq(create_process_with_main_thread(NULL,
	                                             &(const struct process_spawn_params){
													 .name       = "invalid",
													 .user_entry = 0x400000u,
												 }),
	             PROCESS_INVALID_ARGUMENTS);
}

Test(process, spawn_user_creates_main_thread_in_process_address_space) {
	struct process*     process = NULL;
	enum process_result result;

	init_process_test_environment();

	result = create_process_with_main_thread(&process,
	                                         &(const struct process_spawn_params){
												 .name             = "spawned",
												 .user_entry       = 0x400000u,
												 .user_stack_pages = 2u,
												 .preferred_cpu    = NULL,
											 });
	cr_assert_eq(result, PROCESS_OK, "process_create/process_spawn_thread failed: %d", result);
	cr_assert_not_null(process, "process_create did not return a process");
	cr_assert_not_null(process->main_thread, "process should record the main user thread");
	cr_assert_eq(process->main_thread->process, process, "main uthread should point back to the process");
	cr_assert_eq(process->main_thread->thread.address_space,
	             process_address_space(process),
	             "main scheduler thread should run in the process address space");
	cr_assert_eq(process_thread_count(process), 1u, "spawned process should have one thread");
	cr_assert_eq(sched_run_queue_depth(cpu_current()), 1u, "main thread should be runnable");

	thread_mark_zombie(&process->main_thread->thread);
	cr_assert(process_destroy(process), "process_destroy should reclaim terminated main thread");
}
