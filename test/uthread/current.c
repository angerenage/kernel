#include "test_support.h"

Test(uthread, current_returns_running_user_thread) {
	struct process* process = NULL;
	struct uthread* main_thread;

	init_uthread_test_environment();
	process = spawn_owner_process("test/current");
	main_thread = process_main_thread(process);

	cr_assert_null(uthread_current(), "idle scheduler thread should not resolve to a uthread");
	cr_assert_eq(uthread_from_thread(&main_thread->thread), main_thread, "embedded scheduler thread should resolve");

	sched_set_current(cpu_current(), &main_thread->thread);
	cr_assert_eq(uthread_current(), main_thread, "uthread_current should return the running user thread");
	cr_assert_eq(process_current(), process, "process_current should use the running user thread owner");
}

Test(uthread, user_entry_requires_executable_process_mapping) {
	struct process* process;
	struct uthread* main_thread;
	void* executable_base;
	void* data_base;

	init_uthread_test_environment();
	process = spawn_owner_process("test/user-entry");
	main_thread = process_main_thread(process);
	cr_assert(test_address_space_map(process_address_space(process),
			TEST_MAPPING_GRANULE,
			MEMORY_ACCESS_READ | MEMORY_ACCESS_EXEC,
			0u,
			TEST_MAPPING_GRANULE,
			0u,
			NULL,
			&executable_base));
	cr_assert(test_address_space_map(process_address_space(process),
			TEST_MAPPING_GRANULE,
			MEMORY_ACCESS_READ | MEMORY_ACCESS_WRITE,
			0u,
			TEST_MAPPING_GRANULE,
			0u,
			NULL,
			&data_base));

	cr_assert(uthread_user_entry_valid(main_thread, (uintptr_t)executable_base));
	cr_assert_not(uthread_user_entry_valid(main_thread, (uintptr_t)data_base));
	cr_assert_not(uthread_user_entry_valid(main_thread, (uintptr_t)data_base + TEST_MAPPING_GRANULE));
	cr_assert_not(uthread_user_entry_valid(main_thread, 0u));
	cr_assert_not(uthread_user_entry_valid(NULL, (uintptr_t)executable_base));

	terminate_main_thread(process);
	cr_assert(process_destroy(process));
}
