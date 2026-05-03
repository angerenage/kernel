#include <core/address_transfer.h>
#include <core/cpu.h>
#include <core/kheap.h>
#include <core/mm.h>
#include <core/pmm.h>
#include <core/process.h>
#include <core/sched.h>
#include <core/syscall.h>
#include <core/thread.h>
#include <core/uthread.h>
#include <core/vmm.h>
#include <criterion/criterion.h>
#include <hal/clock.h>
#include <hal/cpu.h>
#include <hal/interrupts.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../mocks/hal/cpu_mock.h"
#include "../vmm/test_support.h"

#define SYSCALL_TEST_ARENA_SIZE KiB(2048)
#define SYSCALL_TEST_HEAP_SIZE KiB(64)

static uint8_t syscall_test_arena[SYSCALL_TEST_ARENA_SIZE] __attribute__((aligned(PMM_PAGE_SIZE)));
static uint8_t syscall_test_heap[SYSCALL_TEST_HEAP_SIZE] __attribute__((aligned(PMM_PAGE_SIZE)));
static size_t  syscall_test_heap_offset;

bool kheap_grow_pages(size_t page_count, void** out_base) {
	size_t bytes;
	size_t offset;

	if (out_base == NULL) return false;
	*out_base = NULL;

	bytes = page_count * PMM_PAGE_SIZE;
	for (;;) {
		offset = __atomic_load_n(&syscall_test_heap_offset, __ATOMIC_ACQUIRE);
		if (bytes > SYSCALL_TEST_HEAP_SIZE - offset) return false;
		if (__atomic_compare_exchange_n(
				&syscall_test_heap_offset, &offset, offset + bytes, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
			*out_base = syscall_test_heap + offset;
			return true;
		}
	}
}

static void syscall_test_init_scheduler(void) {
	irq_enable_local();
	cr_assert(cpu_topology_init_bootstrap(0x100000u, 0x104000u), "cpu_topology_init_bootstrap failed");
	cr_assert_not_null(cpu_bsp(), "cpu_bsp returned NULL");
	cpu_bind_current(cpu_bsp());
	cpu_interrupts_set_ready(cpu_current(), false);
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");
}

static void syscall_test_reset_state(void) {
	hal_clock_stop();
	irq_enable_local();
	hal_cpu_local_bind(NULL);
	hal_cpu_mock_reset_kicks();
}

static void syscall_test_init_process_environment(void) {
	const struct mem_range memory_map[] = {
		{
         .base   = (uintptr_t)syscall_test_arena,
         .length = SYSCALL_TEST_ARENA_SIZE,
         .type   = MEM_RANGE_USABLE,
		 },
	};

	syscall_test_heap_offset = 0u;
	hal_cpu_mock_set_context_switch_hook(NULL);
	hal_cpu_mock_set_thread_context_init_result(true);
	hal_cpu_local_bind(NULL);

	syscall_test_init_scheduler();
	cr_assert(cpu_set_state(cpu_current(), CPU_STATE_ONLINE), "cpu_set_state failed");
	mock_paging_reset();
	cr_assert(pmm_init(memory_map, sizeof(memory_map) / sizeof(memory_map[0]), 0), "pmm_init failed");
	cr_assert(vmm_init(), "vmm_init failed");
	cr_assert(kheap_init(), "kheap_init failed");
}

static struct process* syscall_test_spawn_process(const char* name) {
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

static void syscall_test_thread_entry(void* arg) {
	(void)arg;
}

static syscall_result_t syscall_test_create_process_call(const char* name) {
	return syscall_dispatch(SYSCALL_CREATE_PROCESS, (uintptr_t)name, strlen(name) + 1u, 0u, 0u, 0u, 0u);
}

Test(syscall, nop_returns_ok) {
	syscall_result_t result = syscall_dispatch(SYSCALL_NOP, 1u, 2u, 3u, 4u, 5u, 6u);

	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(result.value, 0u);
	cr_assert(syscall_status_is_success(result.status), "nop should return success");
}

Test(syscall, invalid_number_returns_unknown_syscall) {
	syscall_result_t result = syscall_dispatch(UINTPTR_MAX, 0u, 0u, 0u, 0u, 0u, 0u);

	cr_assert_eq(result.status, SYSCALL_STATUS_UNKNOWN_SYSCALL);
	cr_assert_eq(result.value, UINTPTR_MAX);
	cr_assert(syscall_status_is_caller_error(result.status), "unknown syscall should be a caller error");
}

Test(syscall, print_requires_current_process_for_nonzero_buffer) {
	syscall_result_t result;

	syscall_test_init_process_environment();

	result = syscall_dispatch(SYSCALL_PRINT, 0x1000u, 4u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_UNAVAILABLE);
	cr_assert_eq(result.value, 0u);

	result = syscall_dispatch(SYSCALL_PRINT, 0u, 0u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(result.value, 0u);

	syscall_test_reset_state();
}

Test(syscall, print_rejects_invalid_user_buffer) {
	struct process*         process;
	struct uthread*         main_thread;
	struct address_space*   space;
	struct vmm_alloc_params params = {
		.page_count  = 1u,
		.align_pages = 1u,
		.prot        = VMM_PROT_READ | VMM_PROT_WRITE | VMM_PROT_USER,
		.kind        = VMM_KIND_GENERIC,
		.map_flags   = VMM_MAP_LAZY,
	};
	vmm_id_t         buffer_id = VMM_ID_INVALID;
	void*            buffer_base;
	syscall_result_t result;

	syscall_test_init_process_environment();
	process     = syscall_test_spawn_process("syscall/print");
	main_thread = process_main_thread(process);
	cr_assert_not_null(main_thread);
	sched_set_current(cpu_current(), &main_thread->thread);
	space = process_address_space(process);
	cr_assert(vmm_alloc(space, &params, &buffer_id, &buffer_base), "failed to allocate print buffer");

	result = syscall_dispatch(SYSCALL_PRINT, 0u, 1u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(result.value, 0u);

	result = syscall_dispatch(SYSCALL_PRINT, (uintptr_t)buffer_base + PMM_PAGE_SIZE, 1u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(result.value, 0u);

	thread_mark_zombie(&main_thread->thread);
	cr_assert(process_destroy(process), "process_destroy failed");
	syscall_test_reset_state();
}

Test(syscall, sleep_ms_fails_without_clock) {
	syscall_result_t result;

	syscall_test_init_scheduler();

	result = syscall_dispatch(SYSCALL_SLEEP_MS, 1u, 0u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_UNAVAILABLE);
	cr_assert_eq(result.value, 0u);
	cr_assert(syscall_status_is_kernel_error(SYSCALL_STATUS_UNAVAILABLE));

	syscall_test_reset_state();
}

Test(syscall, sleep_ms_rejects_unrepresentable_deadline) {
	syscall_result_t result;

	syscall_test_init_scheduler();
	cr_assert(hal_clock_start(1000u, NULL, NULL), "hal_clock_start failed");
	sched_tick();

	result = syscall_dispatch(SYSCALL_SLEEP_MS, UINTPTR_MAX, 0u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(result.value, 0u, "sleep_ms should report arg0 as problematic");

	syscall_test_reset_state();
}

Test(syscall, tick_count_returns_scheduler_ticks) {
	syscall_result_t result;

	syscall_test_init_scheduler();
	sched_tick();
	sched_tick();

	result = syscall_dispatch(SYSCALL_TICK_COUNT, 0u, 0u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(result.value, 2u);

	syscall_test_reset_state();
}

Test(syscall, process_and_thread_identity_fail_without_current_process) {
	syscall_result_t result;

	syscall_test_init_scheduler();

	result = syscall_dispatch(SYSCALL_GETPID, 0u, 0u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_UNAVAILABLE);
	cr_assert_eq(result.value, 0u);

	result = syscall_dispatch(SYSCALL_GETTID, 0u, 0u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_UNAVAILABLE);
	cr_assert_eq(result.value, 0u);

	syscall_test_reset_state();
}

Test(syscall, process_and_thread_introspection_return_current_values) {
	struct process*  process;
	struct uthread*  main_thread;
	syscall_result_t result;

	syscall_test_init_process_environment();
	process     = syscall_test_spawn_process("syscall/process");
	main_thread = process_main_thread(process);
	cr_assert_not_null(main_thread, "process should have a main thread");

	sched_set_current(cpu_current(), &main_thread->thread);

	result = syscall_dispatch(SYSCALL_GETPID, 0u, 0u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(result.value, (uintptr_t)process_pid(process));

	result = syscall_dispatch(SYSCALL_GET_PROCESS_THREAD_COUNT, 0u, 0u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(result.value, 1u);

	result = syscall_dispatch(SYSCALL_GETTID, 0u, 0u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(result.value, (uintptr_t)uthread_id(main_thread));

	syscall_test_reset_state();
}

Test(syscall, create_process_returns_new_process_pid) {
	syscall_result_t result;
	struct process*  process;

	syscall_test_init_process_environment();

	result = syscall_test_create_process_call("syscall-created");
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_neq(result.value, (uintptr_t)PROCESS_PID_INVALID);

	process = process_lookup((process_id_t)result.value);
	cr_assert_not_null(process, "created process should be registered");
	cr_assert_eq(process_get_state(process), PROCESS_STATE_NEW, "created process should not be runnable yet");
	cr_assert_eq(process_thread_count(process), 0u, "created process should not have a main thread");
	cr_assert_null(process_main_thread(process), "created process should not publish a main thread");

	cr_assert(process_destroy(process), "process_destroy failed");
	syscall_test_reset_state();
}

Test(syscall, create_process_copies_name_from_current_address_space) {
	struct process*         caller;
	struct process*         created;
	struct uthread*         main_thread;
	struct address_space*   space;
	struct vmm_alloc_params params = {
		.page_count  = 1u,
		.align_pages = 1u,
		.prot        = VMM_PROT_READ | VMM_PROT_WRITE | VMM_PROT_USER,
		.kind        = VMM_KIND_GENERIC,
		.map_flags   = VMM_MAP_LAZY,
	};
	vmm_id_t         name_id   = VMM_ID_INVALID;
	void*            name_base = NULL;
	const char       name[]    = "syscall-user-created";
	syscall_result_t result;
	syscall_result_t invalid_result;
	syscall_result_t unterminated_result;

	syscall_test_init_process_environment();
	caller      = syscall_test_spawn_process("syscall/create-caller");
	main_thread = process_main_thread(caller);
	cr_assert_not_null(main_thread);
	sched_set_current(cpu_current(), &main_thread->thread);
	space = process_address_space(caller);
	cr_assert(vmm_alloc(space, &params, &name_id, &name_base), "failed to allocate user name buffer");
	cr_assert_eq(address_space_copy_to(space, (uintptr_t)name_base, name, sizeof(name)), ADDRESS_TRANSFER_OK);

	invalid_result =
		syscall_dispatch(SYSCALL_CREATE_PROCESS, (uintptr_t)name_base + PMM_PAGE_SIZE, sizeof(name), 0u, 0u, 0u, 0u);
	cr_assert_eq(invalid_result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(invalid_result.value, 0u, "bad create_process name pointer should report arg0");

	unterminated_result =
		syscall_dispatch(SYSCALL_CREATE_PROCESS, (uintptr_t)name_base, sizeof(name) - 1u, 0u, 0u, 0u, 0u);
	cr_assert_eq(unterminated_result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(unterminated_result.value, 1u, "unterminated create_process name should report arg1");

	result = syscall_dispatch(SYSCALL_CREATE_PROCESS, (uintptr_t)name_base, sizeof(name), 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	created = process_lookup((process_id_t)result.value);
	cr_assert_not_null(created);
	cr_assert_str_eq(created->name, name, "created process should own the copied name");

	cr_assert(process_destroy(created), "created process_destroy failed");
	thread_mark_zombie(&main_thread->thread);
	cr_assert(process_destroy(caller), "caller process_destroy failed");
	syscall_test_reset_state();
}

Test(syscall, run_process_creates_main_thread_at_entrypoint) {
	syscall_result_t create_result;
	syscall_result_t run_result;
	struct process*  process;
	struct uthread*  main_thread;

	syscall_test_init_process_environment();

	create_result = syscall_test_create_process_call("syscall-run");
	cr_assert_eq(create_result.status, SYSCALL_STATUS_OK);
	process = process_lookup((process_id_t)create_result.value);
	cr_assert_not_null(process);

	run_result = syscall_dispatch(SYSCALL_RUN_PROCESS, create_result.value, 0x400000u, 0x1234u, 2u, 0u, 0u);
	cr_assert_eq(run_result.status, SYSCALL_STATUS_OK);
	main_thread = process_main_thread(process);
	cr_assert_not_null(main_thread, "run_process should create the process main thread");
	cr_assert_eq(run_result.value, (uintptr_t)uthread_id(main_thread));
	cr_assert_eq(process_get_state(process), PROCESS_STATE_RUNNING);
	cr_assert_eq(process_thread_count(process), 1u);
	cr_assert_eq(main_thread->thread.address_space, process_address_space(process));
	cr_assert_eq(sched_run_queue_depth(cpu_current()), 1u, "main thread should be runnable");

	thread_mark_zombie(&main_thread->thread);
	cr_assert(process_destroy(process), "process_destroy failed");
	syscall_test_reset_state();
}

Test(syscall, run_process_rejects_invalid_or_already_started_process) {
	syscall_result_t create_result;
	syscall_result_t run_result;
	struct process*  process;

	syscall_test_init_process_environment();

	run_result = syscall_dispatch(SYSCALL_RUN_PROCESS, UINTPTR_MAX, 0x400000u, 0u, 0u, 0u, 0u);
	cr_assert_eq(run_result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(run_result.value, 0u);

	create_result = syscall_test_create_process_call("syscall-run-invalid");
	cr_assert_eq(create_result.status, SYSCALL_STATUS_OK);
	process = process_lookup((process_id_t)create_result.value);
	cr_assert_not_null(process);

	run_result = syscall_dispatch(SYSCALL_RUN_PROCESS, create_result.value, 0u, 0u, 0u, 0u, 0u);
	cr_assert_eq(run_result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(run_result.value, 1u);

	run_result = syscall_dispatch(SYSCALL_RUN_PROCESS, create_result.value, 0x400000u, 0u, 0u, 0u, 0u);
	cr_assert_eq(run_result.status, SYSCALL_STATUS_OK);
	run_result = syscall_dispatch(SYSCALL_RUN_PROCESS, create_result.value, 0x410000u, 0u, 0u, 0u, 0u);
	cr_assert_eq(run_result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(run_result.value, 0u);

	thread_mark_zombie(&process_main_thread(process)->thread);
	cr_assert(process_destroy(process), "process_destroy failed");
	syscall_test_reset_state();
}

Test(syscall, wait_process_returns_exit_code_and_reclaims_process) {
	syscall_result_t create_result;
	syscall_result_t run_result;
	syscall_result_t wait_result;
	struct process*  process;
	struct uthread*  main_thread;
	process_id_t     pid;

	syscall_test_init_process_environment();

	create_result = syscall_test_create_process_call("syscall-wait");
	cr_assert_eq(create_result.status, SYSCALL_STATUS_OK);
	pid     = (process_id_t)create_result.value;
	process = process_lookup(pid);
	cr_assert_not_null(process);
	run_result = syscall_dispatch(SYSCALL_RUN_PROCESS, create_result.value, 0x400000u, 0u, 0u, 0u, 0u);
	cr_assert_eq(run_result.status, SYSCALL_STATUS_OK);

	cr_assert(process_terminate(process, 42u));
	main_thread = process_main_thread(process);
	thread_mark_zombie(&main_thread->thread);
	process_notify_thread_exit(process, &main_thread->thread, 42u);

	wait_result = syscall_dispatch(SYSCALL_WAIT_PROCESS, create_result.value, 0u, 0u, 0u, 0u, 0u);
	cr_assert_eq(wait_result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(wait_result.value, 42u);
	cr_assert_null(process_lookup(pid), "wait_process should destroy a joined process");

	syscall_test_reset_state();
}

Test(syscall, detach_and_kill_process_dispatch_to_lifecycle_helpers) {
	syscall_result_t create_result;
	syscall_result_t run_result;
	syscall_result_t detach_result;
	syscall_result_t kill_result;
	syscall_result_t wait_result;
	struct process*  process;

	syscall_test_init_process_environment();

	create_result = syscall_test_create_process_call("syscall-kill");
	cr_assert_eq(create_result.status, SYSCALL_STATUS_OK);
	process = process_lookup((process_id_t)create_result.value);
	cr_assert_not_null(process);
	run_result = syscall_dispatch(SYSCALL_RUN_PROCESS, create_result.value, 0x400000u, 0u, 0u, 0u, 0u);
	cr_assert_eq(run_result.status, SYSCALL_STATUS_OK);

	kill_result = syscall_dispatch(SYSCALL_KILL_PROCESS, create_result.value, 7u, 0u, 0u, 0u, 0u);
	cr_assert_eq(kill_result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(process_get_state(process), PROCESS_STATE_EXITING);
	cr_assert(thread_cancel_requested(&process_main_thread(process)->thread));

	detach_result = syscall_dispatch(SYSCALL_DETACH_PROCESS, create_result.value, 0u, 0u, 0u, 0u, 0u);
	cr_assert_eq(detach_result.status, SYSCALL_STATUS_OK);
	wait_result = syscall_dispatch(SYSCALL_WAIT_PROCESS, create_result.value, 0u, 0u, 0u, 0u, 0u);
	cr_assert_eq(wait_result.status, SYSCALL_STATUS_BAD_ARGUMENT);

	thread_mark_zombie(&process_main_thread(process)->thread);
	process_notify_thread_exit(process, &process_main_thread(process)->thread, 7u);
	cr_assert(process_destroy(process), "process_destroy failed");
	syscall_test_reset_state();
}

Test(syscall, thread_lifecycle_requires_current_thread) {
	syscall_result_t result;

	syscall_test_init_process_environment();

	result = syscall_dispatch(SYSCALL_EXIT_THREAD, 0u, 0u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_UNAVAILABLE);

	result = syscall_dispatch(SYSCALL_SPAWN_THREAD, 0x410000u, 0u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_UNAVAILABLE);

	result = syscall_dispatch(SYSCALL_JOIN_THREAD, 1u, 0u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_UNAVAILABLE);

	result = syscall_dispatch(SYSCALL_DETACH_THREAD, 1u, 0u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_UNAVAILABLE);

	result = syscall_dispatch(SYSCALL_CANCEL_THREAD, 1u, 0u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_UNAVAILABLE);

	result = syscall_dispatch(SYSCALL_SET_THREAD_CANCEL_ENABLED, 1u, 0u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_UNAVAILABLE);

	result = syscall_dispatch(SYSCALL_TEST_THREAD_CANCEL, 0u, 0u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_UNAVAILABLE);

	syscall_test_reset_state();
}

Test(syscall, spawn_thread_creates_joinable_thread_in_current_process) {
	struct process*         process;
	struct uthread*         main_thread;
	struct uthread*         worker;
	syscall_result_t        result;
	struct vmm_alloc_params name_params = {
		.page_count  = 1u,
		.align_pages = 1u,
		.prot        = VMM_PROT_READ | VMM_PROT_WRITE | VMM_PROT_USER,
		.kind        = VMM_KIND_GENERIC,
		.map_flags   = VMM_MAP_LAZY,
	};
	vmm_id_t   name_id   = VMM_ID_INVALID;
	void*      name_base = NULL;
	const char name[]    = "syscall-worker";

	syscall_test_init_process_environment();
	process     = syscall_test_spawn_process("syscall/spawn-thread");
	main_thread = process_main_thread(process);
	cr_assert_not_null(main_thread);
	sched_set_current(cpu_current(), &main_thread->thread);
	cr_assert(vmm_alloc(process_address_space(process), &name_params, &name_id, &name_base),
	          "failed to allocate user thread name buffer");
	cr_assert_eq(address_space_copy_to(process_address_space(process), (uintptr_t)name_base, name, sizeof(name)),
	             ADDRESS_TRANSFER_OK);

	result =
		syscall_dispatch(SYSCALL_SPAWN_THREAD, 0x410000u, 0x1234u, 2u, 0u, (uintptr_t)name_base, sizeof(name) - 1u);
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(result.value, 5u, "unterminated spawn_thread name should report arg5");

	result = syscall_dispatch(SYSCALL_SPAWN_THREAD, 0x410000u, 0x1234u, 2u, 0u, (uintptr_t)name_base, sizeof(name));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_neq(result.value, (uintptr_t)UTHREAD_ID_INVALID);
	worker = uthread_lookup((uthread_id_t)result.value);
	cr_assert_not_null(worker, "spawn_thread should return the new TID");
	cr_assert_str_eq(worker->thread.name, name, "spawned thread should own the copied name");
	cr_assert_eq(worker->process, process);
	cr_assert_eq(worker->thread.address_space, process_address_space(process));
	cr_assert_eq(process_thread_count(process), 2u);
	cr_assert_eq(sched_run_queue_depth(cpu_current()), 2u);

	thread_mark_zombie(&worker->thread);
	thread_mark_zombie(&main_thread->thread);
	cr_assert(process_destroy(process), "process_destroy failed");
	syscall_test_reset_state();
}

Test(syscall, spawn_thread_rejects_missing_entrypoint_with_argument_index) {
	struct process*  process;
	struct uthread*  main_thread;
	syscall_result_t result;

	syscall_test_init_process_environment();
	process     = syscall_test_spawn_process("syscall/spawn-invalid");
	main_thread = process_main_thread(process);
	cr_assert_not_null(main_thread);
	sched_set_current(cpu_current(), &main_thread->thread);

	result = syscall_dispatch(SYSCALL_SPAWN_THREAD, 0u, 0u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(result.value, 0u);

	thread_mark_zombie(&main_thread->thread);
	cr_assert(process_destroy(process), "process_destroy failed");
	syscall_test_reset_state();
}

Test(syscall, spawn_thread_rejects_bad_name_pointer_with_argument_index) {
	struct process*  process;
	struct uthread*  main_thread;
	syscall_result_t result;

	syscall_test_init_process_environment();
	process     = syscall_test_spawn_process("syscall/spawn-bad-name");
	main_thread = process_main_thread(process);
	cr_assert_not_null(main_thread);
	sched_set_current(cpu_current(), &main_thread->thread);

	result = syscall_dispatch(SYSCALL_SPAWN_THREAD, 0x410000u, 0u, 0u, 0u, MM_USER_VMM_BASE, 4u);
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(result.value, 4u);

	thread_mark_zombie(&main_thread->thread);
	cr_assert(process_destroy(process), "process_destroy failed");
	syscall_test_reset_state();
}

Test(syscall, join_thread_returns_exit_code_and_reclaims_thread) {
	struct process*  process;
	struct uthread*  main_thread;
	struct uthread*  worker;
	syscall_result_t spawn_result;
	syscall_result_t join_result;

	syscall_test_init_process_environment();
	process     = syscall_test_spawn_process("syscall/join-thread");
	main_thread = process_main_thread(process);
	cr_assert_not_null(main_thread);
	sched_set_current(cpu_current(), &main_thread->thread);

	spawn_result = syscall_dispatch(SYSCALL_SPAWN_THREAD, 0x410000u, 0u, 0u, 0u, 0u, 0u);
	cr_assert_eq(spawn_result.status, SYSCALL_STATUS_OK);
	worker = uthread_lookup((uthread_id_t)spawn_result.value);
	cr_assert_not_null(worker);

	thread_mark_exiting(&worker->thread, 55u);
	thread_mark_zombie(&worker->thread);
	join_result = syscall_dispatch(SYSCALL_JOIN_THREAD, spawn_result.value, 0u, 0u, 0u, 0u, 0u);
	cr_assert_eq(join_result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(join_result.value, 55u);
	cr_assert_null(uthread_lookup((uthread_id_t)spawn_result.value), "joined thread should be reclaimed");
	cr_assert_eq(process_thread_count(process), 1u);

	thread_mark_zombie(&main_thread->thread);
	cr_assert(process_destroy(process), "process_destroy failed");
	syscall_test_reset_state();
}

Test(syscall, thread_target_syscalls_reject_invalid_tid_with_argument_index) {
	struct process*  process;
	struct uthread*  main_thread;
	syscall_result_t result;

	syscall_test_init_process_environment();
	process     = syscall_test_spawn_process("syscall/thread-invalid-target");
	main_thread = process_main_thread(process);
	cr_assert_not_null(main_thread);
	sched_set_current(cpu_current(), &main_thread->thread);

	result = syscall_dispatch(SYSCALL_JOIN_THREAD, UINTPTR_MAX, 0u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(result.value, 0u);

	result = syscall_dispatch(SYSCALL_DETACH_THREAD, UINTPTR_MAX, 0u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(result.value, 0u);

	result = syscall_dispatch(SYSCALL_CANCEL_THREAD, UINTPTR_MAX, 0u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(result.value, 0u);

	thread_mark_zombie(&main_thread->thread);
	cr_assert(process_destroy(process), "process_destroy failed");
	syscall_test_reset_state();
}

Test(syscall, cancel_thread_requests_deferred_cancellation) {
	struct process*  process;
	struct uthread*  main_thread;
	struct uthread*  worker;
	syscall_result_t spawn_result;
	syscall_result_t cancel_result;

	syscall_test_init_process_environment();
	process     = syscall_test_spawn_process("syscall/cancel-thread");
	main_thread = process_main_thread(process);
	cr_assert_not_null(main_thread);
	sched_set_current(cpu_current(), &main_thread->thread);

	spawn_result = syscall_dispatch(SYSCALL_SPAWN_THREAD, 0x410000u, 0u, 0u, 0u, 0u, 0u);
	cr_assert_eq(spawn_result.status, SYSCALL_STATUS_OK);
	worker = uthread_lookup((uthread_id_t)spawn_result.value);
	cr_assert_not_null(worker);

	cancel_result = syscall_dispatch(SYSCALL_CANCEL_THREAD, spawn_result.value, 0u, 0u, 0u, 0u, 0u);
	cr_assert_eq(cancel_result.status, SYSCALL_STATUS_OK);
	cr_assert(thread_cancel_requested(&worker->thread), "target thread should record cancellation");

	thread_mark_zombie(&worker->thread);
	thread_mark_zombie(&main_thread->thread);
	cr_assert(process_destroy(process), "process_destroy failed");
	syscall_test_reset_state();
}

Test(syscall, set_thread_cancel_enabled_updates_current_thread) {
	struct process*  process;
	struct uthread*  main_thread;
	syscall_result_t result;

	syscall_test_init_process_environment();
	process     = syscall_test_spawn_process("syscall/cancel-enable");
	main_thread = process_main_thread(process);
	cr_assert_not_null(main_thread);
	sched_set_current(cpu_current(), &main_thread->thread);
	cr_assert(thread_cancel_enabled(&main_thread->thread));

	result = syscall_dispatch(SYSCALL_SET_THREAD_CANCEL_ENABLED, 0u, 0u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert(!thread_cancel_enabled(&main_thread->thread));

	result = syscall_dispatch(SYSCALL_TEST_THREAD_CANCEL, 0u, 0u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);

	result = syscall_dispatch(SYSCALL_SET_THREAD_CANCEL_ENABLED, 1u, 0u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert(thread_cancel_enabled(&main_thread->thread));

	thread_mark_zombie(&main_thread->thread);
	cr_assert(process_destroy(process), "process_destroy failed");
	syscall_test_reset_state();
}

Test(syscall, address_space_alloc_query_protect_unmap_map_and_free_current_process) {
	struct process*         process;
	struct uthread*         main_thread;
	struct address_space*   space;
	struct vmm_alloc_params out_params = {
		.page_count  = 1u,
		.align_pages = 1u,
		.prot        = VMM_PROT_READ | VMM_PROT_WRITE | VMM_PROT_USER,
		.kind        = VMM_KIND_GENERIC,
		.map_flags   = VMM_MAP_LAZY,
	};
	vmm_id_t         out_id     = VMM_ID_INVALID;
	void*            out_base   = NULL;
	vmm_id_t         alloc_id   = VMM_ID_INVALID;
	uintptr_t        alloc_base = 0u;
	struct vmm_info  info;
	syscall_result_t result;

	syscall_test_init_process_environment();
	process     = syscall_test_spawn_process("syscall/vm-current");
	main_thread = process_main_thread(process);
	cr_assert_not_null(main_thread);
	sched_set_current(cpu_current(), &main_thread->thread);
	space = process_address_space(process);
	cr_assert(vmm_alloc(space, &out_params, &out_id, &out_base), "failed to allocate syscall output page");

	result = syscall_dispatch(SYSCALL_VM_ALLOC,
	                          0u,
	                          2u,
	                          VMM_PROT_READ | VMM_PROT_WRITE | VMM_PROT_USER,
	                          VMM_KIND_GENERIC,
	                          VMM_MAP_LAZY,
	                          (uintptr_t)out_base);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_neq(result.value, (uintptr_t)VMM_ID_INVALID);
	alloc_id = (vmm_id_t)result.value;
	cr_assert_eq(address_space_copy_from(space, (uintptr_t)out_base, &alloc_base, sizeof(alloc_base)),
	             ADDRESS_TRANSFER_OK);
	cr_assert_neq(alloc_base, 0u);

	result = syscall_dispatch(SYSCALL_VM_QUERY, 0u, alloc_id, (uintptr_t)out_base, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(address_space_copy_from(space, (uintptr_t)out_base, &info, sizeof(info)), ADDRESS_TRANSFER_OK);
	cr_assert_eq(info.id, alloc_id);
	cr_assert_eq((uintptr_t)info.base, alloc_base);
	cr_assert_eq(info.page_count, 2u);
	cr_assert_eq(info.state, VMM_STATE_RESERVED);

	result = syscall_dispatch(SYSCALL_VM_PROTECT, 0u, info.id, VMM_PROT_READ | VMM_PROT_USER, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);

	result = syscall_dispatch(SYSCALL_VM_MAP, 0u, info.id, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert(vmm_query_id(space, info.id, &info), "allocation should still be queryable after map");
	cr_assert_eq(info.state, VMM_STATE_MAPPED);
	cr_assert_eq(info.prot, VMM_PROT_READ | VMM_PROT_USER);

	result = syscall_dispatch(SYSCALL_VM_UNMAP, 0u, info.id, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert(vmm_query_id(space, info.id, &info), "allocation should still be queryable after unmap");
	cr_assert_eq(info.state, VMM_STATE_RESERVED);

	result = syscall_dispatch(SYSCALL_VM_MAP, 0u, info.id, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);

	result = syscall_dispatch(SYSCALL_VM_FREE, 0u, info.id, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert(!vmm_query_id(space, info.id, &info), "freed allocation should no longer be tracked");

	thread_mark_zombie(&main_thread->thread);
	cr_assert(process_destroy(process), "process_destroy failed");
	syscall_test_reset_state();
}

Test(syscall, address_space_syscalls_can_target_another_process) {
	struct process*         caller;
	struct process*         target;
	struct uthread*         caller_thread;
	struct uthread*         target_thread;
	struct address_space*   caller_space;
	struct address_space*   target_space;
	struct vmm_alloc_params out_params = {
		.page_count  = 1u,
		.align_pages = 1u,
		.prot        = VMM_PROT_READ | VMM_PROT_WRITE | VMM_PROT_USER,
		.kind        = VMM_KIND_GENERIC,
		.map_flags   = VMM_MAP_LAZY,
	};
	vmm_id_t         out_id = VMM_ID_INVALID;
	void*            out_base;
	vmm_id_t         target_id;
	uintptr_t        target_base = 0u;
	struct vmm_info  info;
	syscall_result_t result;

	syscall_test_init_process_environment();
	caller        = syscall_test_spawn_process("syscall/vm-caller");
	target        = syscall_test_spawn_process("syscall/vm-target");
	caller_thread = process_main_thread(caller);
	target_thread = process_main_thread(target);
	cr_assert_not_null(caller_thread);
	cr_assert_not_null(target_thread);
	sched_set_current(cpu_current(), &caller_thread->thread);
	caller_space = process_address_space(caller);
	target_space = process_address_space(target);
	cr_assert(vmm_alloc(caller_space, &out_params, &out_id, &out_base), "failed to allocate caller output page");

	result = syscall_dispatch(SYSCALL_VM_ALLOC,
	                          process_pid(target),
	                          1u,
	                          VMM_PROT_READ | VMM_PROT_WRITE | VMM_PROT_USER,
	                          VMM_KIND_HEAP,
	                          0u,
	                          (uintptr_t)out_base);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	target_id = (vmm_id_t)result.value;
	cr_assert_eq(address_space_copy_from(caller_space, (uintptr_t)out_base, &target_base, sizeof(target_base)),
	             ADDRESS_TRANSFER_OK);
	cr_assert(vmm_query_id(target_space, target_id, &info), "target allocation should be tracked");
	cr_assert_eq((uintptr_t)info.base, target_base);
	cr_assert_eq(info.kind, VMM_KIND_HEAP);
	cr_assert_eq(info.state, VMM_STATE_MAPPED);

	result = syscall_dispatch(SYSCALL_VM_QUERY, process_pid(target), info.id, (uintptr_t)out_base, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	memset(&info, 0, sizeof(info));
	cr_assert_eq(address_space_copy_from(caller_space, (uintptr_t)out_base, &info, sizeof(info)), ADDRESS_TRANSFER_OK);
	cr_assert_eq(info.id, target_id);
	cr_assert_eq((uintptr_t)info.base, target_base);

	result = syscall_dispatch(SYSCALL_VM_FREE, process_pid(target), info.id, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert(!vmm_query_id(target_space, info.id, &info), "target allocation should be freed");

	thread_mark_zombie(&target_thread->thread);
	thread_mark_zombie(&caller_thread->thread);
	cr_assert(process_destroy(target), "target process_destroy failed");
	cr_assert(process_destroy(caller), "caller process_destroy failed");
	syscall_test_reset_state();
}

Test(syscall, address_space_syscalls_reject_bad_targets_and_arguments) {
	struct process*  process;
	struct uthread*  main_thread;
	syscall_result_t result;

	syscall_test_init_process_environment();

	result = syscall_dispatch(SYSCALL_VM_ALLOC, 0u, 1u, VMM_PROT_READ | VMM_PROT_USER, VMM_KIND_GENERIC, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_UNAVAILABLE);
	cr_assert_eq(result.value, 0u);

	process     = syscall_test_spawn_process("syscall/vm-invalid");
	main_thread = process_main_thread(process);
	cr_assert_not_null(main_thread);
	sched_set_current(cpu_current(), &main_thread->thread);

	result =
		syscall_dispatch(SYSCALL_VM_ALLOC, UINTPTR_MAX, 1u, VMM_PROT_READ | VMM_PROT_USER, VMM_KIND_GENERIC, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(result.value, 0u);

	result = syscall_dispatch(SYSCALL_VM_ALLOC, 0u, 0u, VMM_PROT_READ | VMM_PROT_USER, VMM_KIND_GENERIC, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(result.value, 1u);

	result = syscall_dispatch(SYSCALL_VM_ALLOC, 0u, 1u, VMM_PROT_VALID_MASK << 1u, VMM_KIND_GENERIC, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(result.value, 2u);

	result = syscall_dispatch(SYSCALL_VM_ALLOC, 0u, 1u, VMM_PROT_READ | VMM_PROT_USER, VMM_KIND_KERNEL_DATA, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(result.value, 3u);

	result = syscall_dispatch(SYSCALL_VM_FREE, 0u, VMM_ID_INVALID, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(result.value, 1u);

	result = syscall_dispatch(SYSCALL_VM_QUERY, 0u, 1u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(result.value, 2u);

	thread_mark_zombie(&main_thread->thread);
	cr_assert(process_destroy(process), "process_destroy failed");
	syscall_test_reset_state();
}

Test(syscall, vm_copy_from_and_copy_to_copy_between_current_and_target_process) {
	struct process*         caller;
	struct process*         target;
	struct uthread*         caller_thread;
	struct uthread*         target_thread;
	struct address_space*   caller_space;
	struct address_space*   target_space;
	struct vmm_alloc_params buffer_params = {
		.page_count  = 1u,
		.align_pages = 1u,
		.prot        = VMM_PROT_READ | VMM_PROT_WRITE | VMM_PROT_USER,
		.kind        = VMM_KIND_GENERIC,
	};
	vmm_id_t         caller_id = VMM_ID_INVALID;
	vmm_id_t         target_id = VMM_ID_INVALID;
	void*            caller_base;
	void*            target_base;
	const char       caller_data[] = "caller-to-target";
	const char       target_data[] = "target-to-caller";
	char             copied[sizeof(target_data)];
	syscall_result_t result;

	syscall_test_init_process_environment();
	caller        = syscall_test_spawn_process("syscall/vm-copy-caller");
	target        = syscall_test_spawn_process("syscall/vm-copy-target");
	caller_thread = process_main_thread(caller);
	target_thread = process_main_thread(target);
	cr_assert_not_null(caller_thread);
	cr_assert_not_null(target_thread);
	sched_set_current(cpu_current(), &caller_thread->thread);
	caller_space = process_address_space(caller);
	target_space = process_address_space(target);
	cr_assert(vmm_alloc(caller_space, &buffer_params, &caller_id, &caller_base), "failed to allocate caller buffer");
	cr_assert(vmm_alloc(target_space, &buffer_params, &target_id, &target_base), "failed to allocate target buffer");

	cr_assert_eq(address_space_copy_to(caller_space, (uintptr_t)caller_base, caller_data, sizeof(caller_data)),
	             ADDRESS_TRANSFER_OK);
	result = syscall_dispatch(SYSCALL_VM_COPY_TO,
	                          process_pid(target),
	                          (uintptr_t)target_base,
	                          (uintptr_t)caller_base,
	                          sizeof(caller_data),
	                          0u,
	                          0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(result.value, sizeof(caller_data));
	memset(copied, 0, sizeof(copied));
	cr_assert_eq(address_space_copy_from(target_space, (uintptr_t)target_base, copied, sizeof(caller_data)),
	             ADDRESS_TRANSFER_OK);
	cr_assert_str_eq(copied, caller_data);

	cr_assert_eq(address_space_copy_to(target_space, (uintptr_t)target_base, target_data, sizeof(target_data)),
	             ADDRESS_TRANSFER_OK);
	memset(copied, 0, sizeof(copied));
	cr_assert_eq(address_space_copy_to(caller_space, (uintptr_t)caller_base, copied, sizeof(copied)),
	             ADDRESS_TRANSFER_OK);
	result = syscall_dispatch(SYSCALL_VM_COPY_FROM,
	                          process_pid(target),
	                          (uintptr_t)target_base,
	                          (uintptr_t)caller_base,
	                          sizeof(target_data),
	                          0u,
	                          0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(result.value, sizeof(target_data));
	cr_assert_eq(address_space_copy_from(caller_space, (uintptr_t)caller_base, copied, sizeof(copied)),
	             ADDRESS_TRANSFER_OK);
	cr_assert_str_eq(copied, target_data);

	thread_mark_zombie(&target_thread->thread);
	thread_mark_zombie(&caller_thread->thread);
	cr_assert(process_destroy(target), "target process_destroy failed");
	cr_assert(process_destroy(caller), "caller process_destroy failed");
	syscall_test_reset_state();
}

Test(syscall, vm_copy_from_and_copy_to_reject_bad_targets_and_ranges) {
	struct process*         caller;
	struct process*         target;
	struct uthread*         caller_thread;
	struct uthread*         target_thread;
	struct address_space*   caller_space;
	struct address_space*   target_space;
	struct vmm_alloc_params buffer_params = {
		.page_count  = 1u,
		.align_pages = 1u,
		.prot        = VMM_PROT_READ | VMM_PROT_WRITE | VMM_PROT_USER,
		.kind        = VMM_KIND_GENERIC,
	};
	vmm_id_t         caller_id = VMM_ID_INVALID;
	vmm_id_t         target_id = VMM_ID_INVALID;
	void*            caller_base;
	void*            target_base;
	syscall_result_t result;

	syscall_test_init_process_environment();

	result = syscall_dispatch(SYSCALL_VM_COPY_FROM, 0u, 1u, 2u, 1u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_UNAVAILABLE);
	cr_assert_eq(result.value, 0u);
	result = syscall_dispatch(SYSCALL_VM_COPY_TO, 0u, 1u, 2u, 1u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_UNAVAILABLE);
	cr_assert_eq(result.value, 0u);

	caller        = syscall_test_spawn_process("syscall/vm-copy-invalid-caller");
	target        = syscall_test_spawn_process("syscall/vm-copy-invalid-target");
	caller_thread = process_main_thread(caller);
	target_thread = process_main_thread(target);
	cr_assert_not_null(caller_thread);
	cr_assert_not_null(target_thread);
	sched_set_current(cpu_current(), &caller_thread->thread);
	caller_space = process_address_space(caller);
	target_space = process_address_space(target);
	cr_assert(vmm_alloc(caller_space, &buffer_params, &caller_id, &caller_base), "failed to allocate caller buffer");
	cr_assert(vmm_alloc(target_space, &buffer_params, &target_id, &target_base), "failed to allocate target buffer");

	result =
		syscall_dispatch(SYSCALL_VM_COPY_FROM, UINTPTR_MAX, (uintptr_t)target_base, (uintptr_t)caller_base, 1u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(result.value, 0u);

	result = syscall_dispatch(SYSCALL_VM_COPY_FROM,
	                          process_pid(target),
	                          (uintptr_t)target_base + PMM_PAGE_SIZE,
	                          (uintptr_t)caller_base,
	                          1u,
	                          0u,
	                          0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(result.value, 1u);

	result = syscall_dispatch(SYSCALL_VM_COPY_FROM,
	                          process_pid(target),
	                          (uintptr_t)target_base,
	                          (uintptr_t)caller_base + PMM_PAGE_SIZE,
	                          1u,
	                          0u,
	                          0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(result.value, 2u);

	result = syscall_dispatch(SYSCALL_VM_COPY_TO,
	                          process_pid(target),
	                          (uintptr_t)target_base + PMM_PAGE_SIZE,
	                          (uintptr_t)caller_base,
	                          1u,
	                          0u,
	                          0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(result.value, 1u);

	result = syscall_dispatch(SYSCALL_VM_COPY_TO,
	                          process_pid(target),
	                          (uintptr_t)target_base,
	                          (uintptr_t)caller_base + PMM_PAGE_SIZE,
	                          1u,
	                          0u,
	                          0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(result.value, 2u);

	result = syscall_dispatch(
		SYSCALL_VM_COPY_TO, process_pid(target), (uintptr_t)target_base, (uintptr_t)caller_base, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(result.value, 0u);

	thread_mark_zombie(&target_thread->thread);
	thread_mark_zombie(&caller_thread->thread);
	cr_assert(process_destroy(target), "target process_destroy failed");
	cr_assert(process_destroy(caller), "caller process_destroy failed");
	syscall_test_reset_state();
}

Test(syscall, yield_dispatches_next_runnable_thread) {
	const struct thread_create_params first_params = {
		.name              = "syscall-yield-first",
		.entry             = syscall_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x300000u,
		.kernel_stack_top  = 0x304000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	const struct thread_create_params second_params = {
		.name              = "syscall-yield-second",
		.entry             = syscall_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x310000u,
		.kernel_stack_top  = 0x314000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct thread first;
	struct thread second;

	syscall_test_init_scheduler();
	cr_assert(thread_init(&first, &first_params), "thread_init failed for first thread");
	cr_assert(thread_init(&second, &second_params), "thread_init failed for second thread");
	cr_assert(sched_make_runnable(&first), "failed to make first thread runnable");
	cr_assert(sched_make_runnable(&second), "failed to make second thread runnable");

	cr_assert_eq(syscall_dispatch(SYSCALL_YIELD, 0u, 0u, 0u, 0u, 0u, 0u).status, SYSCALL_STATUS_OK);
	cr_assert_eq(sched_current_thread(), &first, "yield should dispatch first runnable thread");

	cr_assert_eq(syscall_dispatch(SYSCALL_YIELD, 0u, 0u, 0u, 0u, 0u, 0u).status, SYSCALL_STATUS_OK);
	cr_assert_eq(sched_current_thread(), &second, "yield should rotate to the next runnable thread");
	cr_assert(thread_is_queued(&first), "previous thread should be queued after yielding");

	syscall_test_reset_state();
}

Test(syscall, sleep_ms_zero_yields) {
	const struct thread_create_params first_params = {
		.name              = "syscall-sleep-zero-first",
		.entry             = syscall_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x320000u,
		.kernel_stack_top  = 0x324000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	const struct thread_create_params second_params = {
		.name              = "syscall-sleep-zero-second",
		.entry             = syscall_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x330000u,
		.kernel_stack_top  = 0x334000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct thread first;
	struct thread second;

	syscall_test_init_scheduler();
	cr_assert(thread_init(&first, &first_params), "thread_init failed for first thread");
	cr_assert(thread_init(&second, &second_params), "thread_init failed for second thread");
	cr_assert(sched_make_runnable(&first), "failed to make first thread runnable");
	cr_assert(sched_make_runnable(&second), "failed to make second thread runnable");

	sched_yield();
	cr_assert_eq(sched_current_thread(), &first, "first thread should be running before sleep_ms(0)");
	cr_assert_eq(syscall_dispatch(SYSCALL_SLEEP_MS, 0u, 0u, 0u, 0u, 0u, 0u).status, SYSCALL_STATUS_OK);
	cr_assert_eq(sched_current_thread(), &second, "sleep_ms(0) should yield to the next runnable thread");

	syscall_test_reset_state();
}

Test(syscall, sleep_ms_blocks_until_deadline) {
	const struct thread_create_params sleeper_params = {
		.name              = "syscall-sleeper",
		.entry             = syscall_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x340000u,
		.kernel_stack_top  = 0x344000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	const struct thread_create_params worker_params = {
		.name              = "syscall-worker",
		.entry             = syscall_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x350000u,
		.kernel_stack_top  = 0x354000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct thread sleeper;
	struct thread worker;

	syscall_test_init_scheduler();
	cr_assert(hal_clock_start(1000u, NULL, NULL), "hal_clock_start failed");
	cr_assert(thread_init(&sleeper, &sleeper_params), "thread_init failed for sleeper thread");
	cr_assert(thread_init(&worker, &worker_params), "thread_init failed for worker thread");
	worker.timeslice_ticks     = 1u;
	worker.timeslice_remaining = 1u;
	cr_assert(sched_make_runnable(&sleeper), "failed to make sleeper runnable");
	cr_assert(sched_make_runnable(&worker), "failed to make worker runnable");

	sched_yield();
	cr_assert_eq(sched_current_thread(), &sleeper, "sleeper should dispatch first");
	cr_assert_eq(syscall_dispatch(SYSCALL_SLEEP_MS, 2u, 0u, 0u, 0u, 0u, 0u).status, SYSCALL_STATUS_OK);
	cr_assert_eq(sleeper.state, THREAD_STATE_BLOCKED, "sleeper should block while sleeping");
	cr_assert_eq(sleeper.block_reason, THREAD_BLOCK_SLEEP, "sleeper block reason should be sleep");
	cr_assert_eq(sched_current_thread(), &worker, "worker should run after sleeper blocks");

	sched_tick();
	cr_assert_eq(sched_run_queue_depth(cpu_current()), 0u, "sleeper should not wake before deadline");
	sched_tick();
	cr_assert_eq(sleeper.state, THREAD_STATE_READY, "sleeper should be ready after deadline");
	cr_assert_eq(sched_run_queue_depth(cpu_current()), 1u, "sleeper should be queued after wake");
	cr_assert(sched_handle_interrupt_exit(), "interrupt exit should preempt worker once sleeper wakes");
	cr_assert_eq(sched_current_thread(), &sleeper, "sleeper should run after being woken");

	syscall_test_reset_state();
}
