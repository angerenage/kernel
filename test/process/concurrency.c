#include "test_support.h"

struct process_main_start_ctx {
	struct process*                  process;
	uintptr_t                        entry;
	struct uthread*                  thread;
	enum process_thread_spawn_result result;
};

static size_t process_main_start_hook_entries;
static bool   process_main_start_release_first;

static void process_main_start_context_hook(void) {
	size_t entry = __atomic_add_fetch(&process_main_start_hook_entries, 1u, __ATOMIC_ACQ_REL);

	if (entry == 1u) {
		while (!__atomic_load_n(&process_main_start_release_first, __ATOMIC_ACQUIRE)) thrd_yield();
	}
}

static int process_main_start_worker(void* arg) {
	struct process_main_start_ctx* ctx = arg;

	ctx->thread = NULL;
	ctx->result = process_start_main_thread(ctx->process,
	                                        &ctx->thread,
	                                        &(const struct process_thread_params){
												.name       = "main-race",
												.user_entry = ctx->entry,
												.detached   = false,
											});
	return 0;
}

Test(process, start_main_thread_allows_exactly_one_concurrent_claim) {
	struct process*               process = NULL;
	struct process_main_start_ctx first;
	struct process_main_start_ctx second;
	thrd_t                        first_thread;
	thrd_t                        second_thread;
	size_t                        successes;

	init_process_test_environment();
	cr_assert_eq(process_create(&process, "main-race"), PROCESS_OK, "process_create failed");

	first = (struct process_main_start_ctx){
		.process = process,
		.entry   = 0x400000u,
		.thread  = NULL,
	};
	second = (struct process_main_start_ctx){
		.process = process,
		.entry   = 0x410000u,
		.thread  = NULL,
	};

	__atomic_store_n(&process_main_start_hook_entries, 0u, __ATOMIC_RELEASE);
	__atomic_store_n(&process_main_start_release_first, false, __ATOMIC_RELEASE);
	hal_userspace_mock_set_context_init_hook(process_main_start_context_hook);

	cr_assert_eq(thrd_create(&first_thread, process_main_start_worker, &first),
	             thrd_success,
	             "failed to create first process-start worker");

	while (__atomic_load_n(&process_main_start_hook_entries, __ATOMIC_ACQUIRE) == 0u) thrd_yield();

	cr_assert_eq(thrd_create(&second_thread, process_main_start_worker, &second),
	             thrd_success,
	             "failed to create second process-start worker");
	cr_assert_eq(thrd_join(second_thread, NULL), thrd_success, "failed to join second process-start worker");

	__atomic_store_n(&process_main_start_release_first, true, __ATOMIC_RELEASE);
	cr_assert_eq(thrd_join(first_thread, NULL), thrd_success, "failed to join first process-start worker");

	hal_userspace_mock_set_context_init_hook(NULL);

	successes =
		(first.result == PROCESS_THREAD_SPAWN_OK ? 1u : 0u) + (second.result == PROCESS_THREAD_SPAWN_OK ? 1u : 0u);
	cr_assert_eq(successes, 1u, "exactly one concurrent caller may claim the process main thread");
	cr_assert_eq(process_thread_count(process), 1u, "main-thread race must not attach a second user thread");
	cr_assert_not_null(process_main_thread(process), "successful main-thread claim must publish main_thread");

	if (first.thread != NULL) thread_mark_zombie(&first.thread->thread);
	if (second.thread != NULL && second.thread != first.thread) thread_mark_zombie(&second.thread->thread);
	cr_assert(process_destroy(process), "process_destroy failed after concurrent main-thread test");
}
