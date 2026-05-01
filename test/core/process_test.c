#include <core/cpu.h>
#include <core/kheap.h>
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

bool kheap_grow_pages(size_t page_count, void** out_base) {
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
	cr_assert(kheap_init(), "kheap_init failed");
}

static void terminate_main_thread(struct process* process) {
	if (process != NULL && process->main_thread != NULL) thread_mark_zombie(&process->main_thread->thread);
}

static void terminate_process_thread(struct uthread* thread) {
	if (thread != NULL) thread_mark_zombie(&thread->thread);
}

Test(process, spawn_initializes_pid_state_address_space_and_main_thread) {
	struct process*       process = NULL;
	struct address_space* space;
	enum process_result   result;

	init_process_test_environment();

	result = process_spawn(&process,
	                       &(const struct process_spawn_params){
							   .name       = "init",
							   .user_entry = 0x400000u,
						   });
	cr_assert_eq(result, PROCESS_OK, "process_spawn failed: %d", result);
	cr_assert_not_null(process, "process_spawn did not return a process");
	cr_assert_neq(process_pid(process), PROCESS_PID_INVALID, "process pid should be valid");
	cr_assert_eq(process_get_state(process), PROCESS_STATE_RUNNING, "spawned process should start running");
	cr_assert_eq(process_thread_count(process), 1u, "spawned process should have one main thread");
	cr_assert_not_null(process->main_thread, "spawned process should record its main thread");
	cr_assert_eq(
		process_main_thread(process), process->main_thread, "main thread accessor should return the main thread");

	space = process_address_space(process);
	cr_assert_not_null(space, "process address space should be exposed");
	cr_assert(address_space_is_initialized(space), "process address space should be initialized");
	cr_assert_eq(address_space_total_page_count(space), MM_USER_VMM_SIZE / PMM_PAGE_SIZE);
	cr_assert(address_space_free_page_count(space) < MM_USER_VMM_SIZE / PMM_PAGE_SIZE);

	terminate_main_thread(process);
	cr_assert(process_destroy(process), "process_destroy failed");
}

Test(process, spawn_assigns_monotonic_pids) {
	struct process* first  = NULL;
	struct process* second = NULL;
	process_id_t    first_pid;
	process_id_t    second_pid;

	init_process_test_environment();

	cr_assert_eq(process_spawn(&first,
	                           &(const struct process_spawn_params){
								   .name       = "first",
								   .user_entry = 0x400000u,
							   }),
	             PROCESS_OK);
	cr_assert_eq(process_spawn(&second,
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

	cr_assert_eq(process_spawn(NULL,
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

	result = process_spawn(&process,
	                       &(const struct process_spawn_params){
							   .name             = "spawned",
							   .user_entry       = 0x400000u,
							   .user_arg         = 0x1234u,
							   .user_stack_pages = 2u,
							   .preferred_cpu    = NULL,
						   });
	cr_assert_eq(result, PROCESS_OK, "process_spawn failed: %d", result);
	cr_assert_not_null(process, "process_spawn did not return a process");
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

Test(process, create_thread_starts_joinable_thread_in_process_address_space) {
	struct process* process = NULL;
	struct uthread  worker;

	init_process_test_environment();

	cr_assert_eq(process_spawn(&process,
	                           &(const struct process_spawn_params){
								   .name       = "owner",
								   .user_entry = 0x400000u,
							   }),
	             PROCESS_OK);

	cr_assert_eq(process_create_thread(process,
	                                   &worker,
	                                   &(const struct process_thread_params){
										   .name             = "worker",
										   .user_entry       = 0x410000u,
										   .user_arg         = 0x55u,
										   .user_stack_pages = 2u,
										   .preferred_cpu    = NULL,
										   .detached         = false,
									   }),
	             PROCESS_OK);

	cr_assert_eq(worker.process, process, "created thread should retain its owning process");
	cr_assert_eq(worker.thread.address_space,
	             process_address_space(process),
	             "created thread should use the process address space");
	cr_assert_eq(process_thread_count(process), 2u, "process should track the main thread and created thread");
	cr_assert_eq(sched_run_queue_depth(cpu_current()), 2u, "main and created threads should be runnable");

	terminate_process_thread(&worker);
	terminate_main_thread(process);
	cr_assert(process_destroy(process), "process_destroy should reclaim terminated process threads");
}

Test(process, create_thread_rejects_missing_arguments) {
	struct process* process = NULL;
	struct uthread  worker;

	init_process_test_environment();

	cr_assert_eq(process_spawn(&process,
	                           &(const struct process_spawn_params){
								   .name       = "owner",
								   .user_entry = 0x400000u,
							   }),
	             PROCESS_OK);

	cr_assert_eq(process_create_thread(NULL,
	                                   &worker,
	                                   &(const struct process_thread_params){
										   .name       = "worker",
										   .user_entry = 0x410000u,
									   }),
	             PROCESS_INVALID_ARGUMENTS);
	cr_assert_eq(process_create_thread(process,
	                                   NULL,
	                                   &(const struct process_thread_params){
										   .name       = "worker",
										   .user_entry = 0x410000u,
									   }),
	             PROCESS_INVALID_ARGUMENTS);
	cr_assert_eq(process_create_thread(process, &worker, NULL), PROCESS_INVALID_ARGUMENTS);

	terminate_main_thread(process);
	cr_assert(process_destroy(process), "process_destroy should reclaim terminated main thread");
}

Test(process, destroy_rejects_live_main_thread) {
	struct process* process = NULL;

	init_process_test_environment();

	cr_assert_eq(process_spawn(&process,
	                           &(const struct process_spawn_params){
								   .name       = "live",
								   .user_entry = 0x400000u,
							   }),
	             PROCESS_OK);
	cr_assert(!process_destroy(process), "process_destroy must reject live process threads");

	thread_mark_zombie(&process->main_thread->thread);
	cr_assert(process_destroy(process), "process_destroy should succeed after main thread exits");
}
