#include <base/heap.h>
#include <core/cpu.h>
#include <core/mm.h>
#include <core/pmm.h>
#include <core/process.h>
#include <core/sched.h>
#include <core/thread.h>
#include <core/uthread.h>
#include <core/vaddr_alloc.h>
#include <core/vmm.h>
#include <criterion/criterion.h>
#include <hal/cpu.h>
#include <hal/interrupts.h>
#include <stddef.h>
#include <stdint.h>

#include "../mocks/hal/cpu_mock.h"
#include "../vmm/test_support.h"

#define PROCESS_TEST_ARENA_SIZE KiB(2048)
#define PROCESS_TEST_HEAP_SIZE KiB(64)

static uint8_t process_test_arena[PROCESS_TEST_ARENA_SIZE] __attribute__((aligned(PMM_PAGE_SIZE)));
static uint8_t process_test_heap[PROCESS_TEST_HEAP_SIZE] __attribute__((aligned(PMM_PAGE_SIZE)));
static size_t  process_test_heap_offset;

bool heap_grow_pages(size_t page_count, void** out_base) {
	size_t bytes;
	size_t offset;

	if (out_base == NULL) return false;
	*out_base = NULL;

	bytes = page_count * PMM_PAGE_SIZE;
	for (;;) {
		offset = __atomic_load_n(&process_test_heap_offset, __ATOMIC_ACQUIRE);
		if (bytes > PROCESS_TEST_HEAP_SIZE - offset) return false;
		if (__atomic_compare_exchange_n(
				&process_test_heap_offset, &offset, offset + bytes, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
			*out_base = process_test_heap + offset;
			return true;
		}
	}
}

static void init_bound_bootstrap_cpu(void) {
	irq_enable_local();
	cr_assert(cpu_topology_init_bootstrap(0x100000u, 0x104000u), "cpu_topology_init_bootstrap failed");
	cr_assert_not_null(cpu_bsp(), "cpu_bsp returned NULL");
	cpu_bind_current(cpu_bsp());
	cr_assert(cpu_set_state(cpu_current(), CPU_STATE_ONLINE), "cpu_set_state failed");
	cpu_interrupts_set_ready(cpu_current(), false);
}

static void init_process_test_environment(void) {
	const struct mem_range memory_map[] = {
		{
         .base   = (uintptr_t)process_test_arena,
         .length = PROCESS_TEST_ARENA_SIZE,
         .type   = MEM_RANGE_USABLE,
		 },
	};

	process_test_heap_offset = 0u;
	hal_cpu_mock_set_context_switch_hook(NULL);
	hal_cpu_mock_set_thread_context_init_result(true);
	hal_cpu_local_bind(NULL);

	init_bound_bootstrap_cpu();
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");
	mock_paging_reset();
	cr_assert(pmm_init(memory_map, sizeof(memory_map) / sizeof(memory_map[0]), 0), "pmm_init failed");
	cr_assert(vmm_init(), "vmm_init failed");
	cr_assert(heap_init(), "heap_init failed");
}

static enum process_result process_test_result_from_thread_spawn(enum process_thread_spawn_result result) {
	switch (result) {
	case PROCESS_THREAD_SPAWN_OK:
		return PROCESS_OK;
	case PROCESS_THREAD_SPAWN_INVALID_ARGUMENTS:
		return PROCESS_INVALID_ARGUMENTS;
	case PROCESS_THREAD_SPAWN_NO_MEMORY:
		return PROCESS_NO_MEMORY;
	case PROCESS_THREAD_SPAWN_STACK_ALLOC_FAILED:
		return PROCESS_THREAD_STACK_ALLOC_FAILED;
	case PROCESS_THREAD_SPAWN_CONTEXT_UNSUPPORTED:
		return PROCESS_THREAD_CONTEXT_UNSUPPORTED;
	case PROCESS_THREAD_SPAWN_SCHEDULER_REJECTED:
		return PROCESS_THREAD_SCHEDULER_REJECTED;
	case PROCESS_THREAD_SPAWN_REAPER_UNAVAILABLE:
		return PROCESS_THREAD_REAPER_UNAVAILABLE;
	case PROCESS_THREAD_SPAWN_ID_EXHAUSTED:
	default:
		return PROCESS_THREAD_ID_EXHAUSTED;
	}
}

static enum process_result create_process_with_main_thread(struct process**                   out_process,
                                                           const struct process_spawn_params* params) {
	struct process*                  process = NULL;
	struct uthread*                  main_thread;
	enum process_result              process_result;
	enum process_thread_spawn_result thread_result;

	if (out_process == NULL || params == NULL) return PROCESS_INVALID_ARGUMENTS;
	*out_process = NULL;

	process_result = process_create(&process, params->name);
	if (process_result != PROCESS_OK) return process_result;

	thread_result = process_spawn_thread(process,
	                                     &main_thread,
	                                     &(const struct process_thread_params){
											 .name             = params->name,
											 .user_entry       = params->user_entry,
											 .user_arg         = params->user_arg,
											 .user_stack_pages = params->user_stack_pages,
											 .preferred_cpu    = params->preferred_cpu,
											 .detached         = false,
										 });
	if (thread_result != PROCESS_THREAD_SPAWN_OK) {
		(void)process_destroy(process);
		return process_test_result_from_thread_spawn(thread_result);
	}

	*out_process = process;
	return PROCESS_OK;
}

static void terminate_main_thread(struct process* process) {
	if (process != NULL && process->main_thread != NULL) thread_mark_zombie(&process->main_thread->thread);
}

static void terminate_process_thread(struct uthread* thread) {
	if (thread != NULL) thread_mark_zombie(&thread->thread);
}

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
												 .user_arg         = 0x1234u,
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
										  .user_arg         = 0x66u,
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

	cr_assert(process_terminate(process, 77u), "process_terminate should accept a running process");
	cr_assert_eq(process_get_state(process), PROCESS_STATE_EXITING, "process should enter EXITING state");
	cr_assert_eq(process->exit_code, 77u, "process_terminate should publish the process exit code");
	cr_assert(thread_cancel_requested(&process->main_thread->thread), "process threads should receive cancellation");

	thread_mark_zombie(&process->main_thread->thread);
	process_notify_thread_exit(process, &process->main_thread->thread, 77u);
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
