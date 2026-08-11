#include "test_support.h"

Test(uthread, current_returns_running_user_thread) {
	struct process* process = NULL;
	struct uthread* main_thread;

	init_uthread_test_environment();
	process     = spawn_owner_process("test/current");
	main_thread = process_main_thread(process);

	cr_assert_null(uthread_current(), "idle scheduler thread should not resolve to a uthread");
	cr_assert_eq(uthread_from_thread(&main_thread->thread), main_thread, "embedded scheduler thread should resolve");

	sched_set_current(cpu_current(), &main_thread->thread);
	cr_assert_eq(uthread_current(), main_thread, "uthread_current should return the running user thread");
	cr_assert_eq(process_current(), process, "process_current should use the running user thread owner");
}
