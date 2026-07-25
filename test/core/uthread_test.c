#include <base/heap.h>
#include <core/address_transfer.h>
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

#include "../mocks/hal/cpu_mock.h"
#include "../vmm/test_support.h"

#define UTHREAD_TEST_ARENA_SIZE KiB(2048)
#define UTHREAD_TEST_HEAP_SIZE KiB(64)

static uint8_t uthread_test_arena[UTHREAD_TEST_ARENA_SIZE] __attribute__((aligned(PMM_PAGE_SIZE)));
static uint8_t uthread_test_heap[UTHREAD_TEST_HEAP_SIZE] __attribute__((aligned(PMM_PAGE_SIZE)));
static size_t  uthread_test_heap_offset;

bool heap_grow_pages(size_t page_count, void** out_base) {
	size_t bytes;
	size_t offset;

	if (out_base == NULL) return false;
	*out_base = NULL;

	bytes = page_count * PMM_PAGE_SIZE;
	for (;;) {
		offset = __atomic_load_n(&uthread_test_heap_offset, __ATOMIC_ACQUIRE);
		if (bytes > UTHREAD_TEST_HEAP_SIZE - offset) return false;
		if (__atomic_compare_exchange_n(
				&uthread_test_heap_offset, &offset, offset + bytes, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
			*out_base = uthread_test_heap + offset;
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

static void init_uthread_test_environment(void) {
	const struct mem_range memory_map[] = {
		{
         .base   = (uintptr_t)uthread_test_arena,
         .length = UTHREAD_TEST_ARENA_SIZE,
         .type   = MEM_RANGE_USABLE,
		 },
	};

	uthread_test_heap_offset = 0u;
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

static struct process* spawn_owner_process(const char* name) {
	struct process* process = NULL;
	struct uthread* main_thread;

	cr_assert_eq(process_create(&process, name), PROCESS_OK, "process_create failed");
	cr_assert_not_null(process, "process_create returned NULL process");
	cr_assert_eq(process_spawn_thread(process,
	                                  &main_thread,
	                                  &(const struct process_thread_params){
										  .name       = name,
										  .user_entry = 0x300000u,
										  .detached   = false,
									  }),
	             PROCESS_THREAD_SPAWN_OK,
	             "process_spawn_thread failed");
	return process;
}

static void terminate_main_thread(struct process* process) {
	if (process != NULL && process->main_thread != NULL) thread_mark_zombie(&process->main_thread->thread);
}

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

Test(uthread, start_copies_argument_onto_new_user_stack) {
	struct process* process = NULL;
	struct uthread  worker  = {
		  .user_stack_id   = VMM_ID_INVALID,
		  .kernel_stack_id = VMM_ID_INVALID,
    };
	const struct {
		uint64_t first;
		uint64_t second;
	} source = {.first = 0x1122334455667788ull, .second = 0x8877665544332211ull};
	struct {
		uint64_t first;
		uint64_t second;
	} copied = {0};
	uintptr_t arg_address;

	init_uthread_test_environment();
	process = spawn_owner_process("test/argument-copy");

	cr_assert_eq(uthread_start(&worker,
	                           &(const struct uthread_start_params){
								   .name             = "user/argument-copy",
								   .process          = process,
								   .user_entry       = 0x400000u,
								   .arg_data         = &source,
								   .arg_size         = sizeof(source),
								   .user_stack_pages = 2u,
								   .detached         = false,
							   }),
	             UTHREAD_START_OK);

	arg_address = worker.thread.context.spill[1];
	cr_assert_neq(arg_address, 0u, "new thread should receive an argument pointer");
	cr_assert_neq(arg_address, (uintptr_t)&source, "new thread must not receive the creator's pointer");
	cr_assert_eq(address_space_copy_from(process_address_space(process), arg_address, &copied, sizeof(copied)),
	             ADDRESS_TRANSFER_OK);
	cr_assert_eq(copied.first, source.first);
	cr_assert_eq(copied.second, source.second);

	thread_mark_zombie(&worker.thread);
	cr_assert(uthread_deinit(&worker));
	terminate_main_thread(process);
	cr_assert(process_destroy(process));
}
