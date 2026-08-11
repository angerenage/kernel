#include <base/cap.h>
#include <base/channel.h>
#include <base/heap.h>
#include <base/message.h>
#include <core/address_transfer.h>
#include <core/capability.h>
#include <core/capability_call.h>
#include <core/channel.h>
#include <core/cpu.h>
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

#include "../capability/syscall_test_support.h"
#include "../mocks/hal/cpu_mock.h"
#include "../vmm/test_support.h"

#define SYSCALL_TEST_ARENA_SIZE KiB(2048)
#define SYSCALL_TEST_HEAP_SIZE KiB(256)

static uint8_t syscall_test_arena[SYSCALL_TEST_ARENA_SIZE] __attribute__((aligned(PMM_PAGE_SIZE)));
static uint8_t syscall_test_heap[SYSCALL_TEST_HEAP_SIZE] __attribute__((aligned(PMM_PAGE_SIZE)));
static size_t  syscall_test_heap_offset;

bool heap_grow_pages(size_t page_count, void** out_base) {
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

void syscall_test_reset_state(void) {
	hal_clock_stop();
	irq_enable_local();
	hal_cpu_local_bind(NULL);
	hal_cpu_mock_reset_kicks();
}

void syscall_test_init_process_environment(void) {
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
	cr_assert(heap_init(), "heap_init failed");
}

struct process* syscall_test_spawn_process(const char* name) {
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

struct syscall_test_cap_request {
	uint32_t value;
};

struct syscall_test_cap_response {
	uint32_t value;
};

static syscall_result_t syscall_test_cap_handler(const struct cap_request* request) {
	struct syscall_test_cap_request  input;
	struct syscall_test_cap_response output;

	if (request == NULL || request->request == NULL ||
	    request->request_size < sizeof(struct syscall_test_cap_request)) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	if (request->response == NULL || request->response_capacity < sizeof(output)) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	memcpy(&input, request->request, sizeof(input));
	output.value = input.value + 1u;
	memcpy(request->response, &output, sizeof(output));
	return syscall_result_ok(sizeof(output));
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

Test(syscall, capability_call_uses_distinct_request_and_response_buffers) {
	struct process*                  process;
	struct uthread*                  main_thread;
	struct cap_object*               object;
	struct capability*               capability;
	struct syscall_test_cap_request  request  = {.value = 41u};
	struct syscall_test_cap_response response = {0};
	syscall_result_t                 result;

	syscall_test_init_process_environment();
	capability_init();
	process     = syscall_test_spawn_process("syscall/cap-call");
	main_thread = process_main_thread(process);
	cr_assert_not_null(main_thread);
	sched_set_current(cpu_current(), &main_thread->thread);
	main_thread->thread.address_space = NULL;

	object = cap_object_create_kernel(1u, syscall_test_cap_handler);
	cr_assert_not_null(object);
	capability = cap_create(object->cap_object_id, process_pid(process), CAP_CALL, NULL);
	cr_assert_not_null(capability);

	result = syscall_dispatch(SYSCALL_CAP_CALL,
	                          capability->cap_id,
	                          (uintptr_t)&request,
	                          sizeof(request),
	                          (uintptr_t)&response,
	                          sizeof(response),
	                          0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(result.value, sizeof(response));
	cr_assert_eq(response.value, 42u);

	result = syscall_dispatch(SYSCALL_CAP_CALL, capability->cap_id, 0u, 0u, (uintptr_t)&response, sizeof(response), 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(result.value, 2u);

	result = syscall_dispatch(
		SYSCALL_CAP_CALL, capability->cap_id, (uintptr_t)&request, sizeof(request), 0u, sizeof(response), 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(result.value, 3u);

	cr_assert(cap_destroy(capability));
	cr_assert(cap_object_destroy(object));
	syscall_test_reset_state();
}

Test(syscall, capability_reply_completes_provider_owned_call) {
	struct process*          process;
	struct uthread*          main_thread;
	struct cap_pending_call* pending;
	const uint32_t           reply_value = 0x55aa55aau;
	const void*              response;
	syscall_result_t         result;
	syscall_result_t         call_result;

	syscall_test_init_process_environment();
	capability_init();
	process     = syscall_test_spawn_process("syscall/cap-reply");
	main_thread = process_main_thread(process);
	cr_assert_not_null(main_thread);
	sched_set_current(cpu_current(), &main_thread->thread);
	main_thread->thread.address_space = NULL;

	pending = cap_pending_call_create(9u, process_pid(process), 99u, sizeof(reply_value));
	cr_assert_not_null(pending);
	result = syscall_dispatch(SYSCALL_CAP_REPLY,
	                          cap_pending_call_id(pending),
	                          (uintptr_t)&reply_value,
	                          sizeof(reply_value),
	                          SYSCALL_STATUS_OK,
	                          0u,
	                          0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cap_pending_call_wait(pending, &call_result, &response);
	cr_assert_eq(call_result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(call_result.value, sizeof(reply_value));
	cr_assert_eq(*(const uint32_t*)response, reply_value);
	cap_pending_call_destroy(pending);
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

Test(syscall, exit_thread_requires_current_userspace_thread) {
	syscall_result_t result;

	syscall_test_init_process_environment();
	result = syscall_dispatch(SYSCALL_EXIT_THREAD, 0u, 0u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_UNAVAILABLE);
	syscall_test_reset_state();
}

Test(syscall, channel_create_and_destroy_manage_process_owned_state) {
	struct process*         process;
	struct uthread*         main_thread;
	struct address_space*   space;
	struct channel*         channel;
	struct vmm_alloc_params params = {
		.page_count  = 1u,
		.align_pages = 1u,
		.prot        = VMM_PROT_READ | VMM_PROT_WRITE | VMM_PROT_USER,
		.kind        = VMM_KIND_GENERIC,
		.map_flags   = VMM_MAP_LAZY,
	};
	vmm_id_t         output_id = VMM_ID_INVALID;
	void*            output_base;
	channel_id_t     channel_id = CHANNEL_ID_INVALID;
	syscall_result_t result;

	syscall_test_init_process_environment();
	capability_init();
	process     = syscall_test_spawn_process("syscall/channel");
	main_thread = process_main_thread(process);
	cr_assert_not_null(main_thread);
	sched_set_current(cpu_current(), &main_thread->thread);
	space = process_address_space(process);

	result = syscall_dispatch(SYSCALL_CHANNEL_CREATE, 0u, 0u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(result.value, 0u);
	cr_assert_eq(process->channel_state.count, 0u);

	result = syscall_dispatch(SYSCALL_CHANNEL_CREATE, MM_USER_VMM_BASE, 0u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(result.value, 0u);
	cr_assert_eq(process->channel_state.count, 0u, "failed copyout must roll back channel ownership");

	cr_assert(vmm_alloc(space, &params, &output_id, &output_base), "failed to allocate channel ID output");
	result = syscall_dispatch(SYSCALL_CHANNEL_CREATE, (uintptr_t)output_base, 0u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(address_space_copy_from(space, (uintptr_t)output_base, &channel_id, sizeof(channel_id)),
	             ADDRESS_TRANSFER_OK);
	cr_assert_neq(channel_id, CHANNEL_ID_INVALID);
	cr_assert_eq(process->channel_state.count, 1u);
	channel = channel_acquire(channel_id);
	cr_assert_not_null(channel);
	cr_assert_eq(channel->owner_pid, process_pid(process));
	channel_release(channel);

	result = syscall_dispatch(SYSCALL_CHANNEL_DESTROY, CHANNEL_ID_INVALID, 0u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(result.value, 0u);

	result = syscall_dispatch(SYSCALL_CHANNEL_DESTROY, channel_id, 0u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(process->channel_state.count, 0u);
	cr_assert_null(channel_acquire(channel_id));

	result = syscall_dispatch(SYSCALL_CHANNEL_DESTROY, channel_id, 0u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);

	thread_mark_zombie(&main_thread->thread);
	cr_assert(process_destroy(process), "process_destroy failed");
	syscall_test_reset_state();
}

Test(syscall, message_send_rejects_invalid_pid_and_size) {
	struct process*  process;
	struct uthread*  main_thread;
	syscall_result_t result;

	syscall_test_init_process_environment();
	process     = syscall_test_spawn_process("syscall/message-invalid");
	main_thread = process_main_thread(process);
	cr_assert_not_null(main_thread);
	sched_set_current(cpu_current(), &main_thread->thread);

	result = syscall_dispatch(SYSCALL_SEND_MESSAGE, UINTPTR_MAX, 0u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_FAILED);
	cr_assert_eq(result.value, MESSAGE_INVALID_PID);

	result = syscall_dispatch(SYSCALL_SEND_MESSAGE, process_pid(process), 0u, MESSAGE_MAX_SIZE + 1u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_FAILED);
	cr_assert_eq(result.value, MESSAGE_TOO_LARGE);

	thread_mark_zombie(&main_thread->thread);
	cr_assert(process_destroy(process), "process_destroy failed");
	syscall_test_reset_state();
}

Test(syscall, message_recv_reports_empty_queue) {
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
	vmm_id_t         length_id = VMM_ID_INVALID;
	vmm_id_t         sender_id = VMM_ID_INVALID;
	void*            buffer_base;
	void*            length_base;
	void*            sender_base;
	uintptr_t        length_value = 42u;
	uintptr_t        sender_value = 77u;
	syscall_result_t result;

	syscall_test_init_process_environment();
	process     = syscall_test_spawn_process("syscall/message-empty");
	main_thread = process_main_thread(process);
	cr_assert_not_null(main_thread);
	sched_set_current(cpu_current(), &main_thread->thread);
	space = process_address_space(process);
	cr_assert(vmm_alloc(space, &params, &buffer_id, &buffer_base), "failed to allocate recv buffer");
	cr_assert(vmm_alloc(space, &params, &length_id, &length_base), "failed to allocate length buffer");
	cr_assert(vmm_alloc(space, &params, &sender_id, &sender_base), "failed to allocate sender buffer");
	cr_assert_eq(address_space_write_uintptr(space, (uintptr_t)length_base, length_value), ADDRESS_TRANSFER_OK);
	cr_assert_eq(address_space_write_uintptr(space, (uintptr_t)sender_base, sender_value), ADDRESS_TRANSFER_OK);

	result = syscall_dispatch(SYSCALL_RECV_MESSAGE,
	                          (uintptr_t)buffer_base,
	                          (uintptr_t)length_base,
	                          MESSAGE_MAX_SIZE,
	                          (uintptr_t)sender_base,
	                          0u,
	                          0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(result.value, 0u);
	cr_assert_eq(address_space_read_uintptr(space, (uintptr_t)length_base, &length_value), ADDRESS_TRANSFER_OK);
	cr_assert_eq(length_value, 42u, "empty recv should not update length");
	cr_assert_eq(address_space_read_uintptr(space, (uintptr_t)sender_base, &sender_value), ADDRESS_TRANSFER_OK);
	cr_assert_eq(sender_value, 77u, "empty recv should not update sender");

	thread_mark_zombie(&main_thread->thread);
	cr_assert(process_destroy(process), "process_destroy failed");
	syscall_test_reset_state();
}

Test(syscall, message_send_and_recv_roundtrip) {
	struct process*         caller;
	struct process*         target;
	struct uthread*         caller_thread;
	struct uthread*         target_thread;
	struct address_space*   caller_space;
	struct address_space*   target_space;
	struct vmm_alloc_params params = {
		.page_count  = 1u,
		.align_pages = 1u,
		.prot        = VMM_PROT_READ | VMM_PROT_WRITE | VMM_PROT_USER,
		.kind        = VMM_KIND_GENERIC,
		.map_flags   = VMM_MAP_LAZY,
	};
	vmm_id_t         send_id     = VMM_ID_INVALID;
	vmm_id_t         recv_id     = VMM_ID_INVALID;
	vmm_id_t         len_id      = VMM_ID_INVALID;
	vmm_id_t         sender_id   = VMM_ID_INVALID;
	void*            send_base   = NULL;
	void*            recv_base   = NULL;
	void*            len_base    = NULL;
	void*            sender_base = NULL;
	const char       payload[]   = "syscall-message";
	char             received[sizeof(payload)];
	uintptr_t        length_value = 0u;
	uintptr_t        sender_value = 0u;
	syscall_result_t result;

	syscall_test_init_process_environment();
	caller        = syscall_test_spawn_process("syscall/message-caller");
	target        = syscall_test_spawn_process("syscall/message-target");
	caller_thread = process_main_thread(caller);
	target_thread = process_main_thread(target);
	cr_assert_not_null(caller_thread);
	cr_assert_not_null(target_thread);
	caller_space = process_address_space(caller);
	target_space = process_address_space(target);

	cr_assert(vmm_alloc(caller_space, &params, &send_id, &send_base), "failed to allocate send buffer");
	cr_assert_eq(address_space_copy_to(caller_space, (uintptr_t)send_base, payload, sizeof(payload)),
	             ADDRESS_TRANSFER_OK);

	sched_set_current(cpu_current(), &caller_thread->thread);
	result =
		syscall_dispatch(SYSCALL_SEND_MESSAGE, process_pid(target), (uintptr_t)send_base, sizeof(payload), 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(result.value, 0u);

	cr_assert(vmm_alloc(target_space, &params, &recv_id, &recv_base), "failed to allocate recv buffer");
	cr_assert(vmm_alloc(target_space, &params, &len_id, &len_base), "failed to allocate length buffer");
	cr_assert(vmm_alloc(target_space, &params, &sender_id, &sender_base), "failed to allocate sender buffer");
	cr_assert_eq(address_space_write_uintptr(target_space, (uintptr_t)len_base, 0u), ADDRESS_TRANSFER_OK);
	cr_assert_eq(address_space_write_uintptr(target_space, (uintptr_t)sender_base, 0u), ADDRESS_TRANSFER_OK);

	sched_set_current(cpu_current(), &target_thread->thread);
	result = syscall_dispatch(SYSCALL_RECV_MESSAGE,
	                          (uintptr_t)recv_base,
	                          (uintptr_t)len_base,
	                          MESSAGE_MAX_SIZE,
	                          (uintptr_t)sender_base,
	                          0u,
	                          0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(result.value, 1u);
	cr_assert_eq(address_space_read_uintptr(target_space, (uintptr_t)len_base, &length_value), ADDRESS_TRANSFER_OK);
	cr_assert_eq(length_value, sizeof(payload));
	cr_assert_eq(address_space_read_uintptr(target_space, (uintptr_t)sender_base, &sender_value), ADDRESS_TRANSFER_OK);
	cr_assert_eq(sender_value, (uintptr_t)process_pid(caller));
	cr_assert_eq(address_space_copy_from(target_space, (uintptr_t)recv_base, received, sizeof(received)),
	             ADDRESS_TRANSFER_OK);
	cr_assert_eq(memcmp(received, payload, sizeof(payload)), 0);

	cr_assert_eq(address_space_write_uintptr(target_space, (uintptr_t)sender_base, 88u), ADDRESS_TRANSFER_OK);
	result = syscall_dispatch(SYSCALL_RECV_MESSAGE,
	                          (uintptr_t)recv_base,
	                          (uintptr_t)len_base,
	                          MESSAGE_MAX_SIZE,
	                          (uintptr_t)sender_base,
	                          0u,
	                          0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(result.value, 0u);
	cr_assert_eq(address_space_read_uintptr(target_space, (uintptr_t)sender_base, &sender_value), ADDRESS_TRANSFER_OK);
	cr_assert_eq(sender_value, 88u, "empty recv should not update sender");

	thread_mark_zombie(&target_thread->thread);
	thread_mark_zombie(&caller_thread->thread);
	cr_assert(process_destroy(target), "target process_destroy failed");
	cr_assert(process_destroy(caller), "caller process_destroy failed");
	syscall_test_reset_state();
}

Test(syscall, message_recv_reports_required_size_when_buffer_too_small) {
	struct process*         caller;
	struct process*         target;
	struct uthread*         caller_thread;
	struct uthread*         target_thread;
	struct address_space*   caller_space;
	struct address_space*   target_space;
	struct vmm_alloc_params params = {
		.page_count  = 1u,
		.align_pages = 1u,
		.prot        = VMM_PROT_READ | VMM_PROT_WRITE | VMM_PROT_USER,
		.kind        = VMM_KIND_GENERIC,
		.map_flags   = VMM_MAP_LAZY,
	};
	vmm_id_t         send_id     = VMM_ID_INVALID;
	vmm_id_t         recv_id     = VMM_ID_INVALID;
	vmm_id_t         len_id      = VMM_ID_INVALID;
	vmm_id_t         sender_id   = VMM_ID_INVALID;
	void*            send_base   = NULL;
	void*            recv_base   = NULL;
	void*            len_base    = NULL;
	void*            sender_base = NULL;
	const char       payload[]   = "syscall-message";
	char             received[sizeof(payload)];
	uintptr_t        length_value = 0u;
	uintptr_t        sender_value = 55u;
	syscall_result_t result;

	syscall_test_init_process_environment();
	caller        = syscall_test_spawn_process("syscall/message-small-buffer-caller");
	target        = syscall_test_spawn_process("syscall/message-small-buffer-target");
	caller_thread = process_main_thread(caller);
	target_thread = process_main_thread(target);
	cr_assert_not_null(caller_thread);
	cr_assert_not_null(target_thread);
	caller_space = process_address_space(caller);
	target_space = process_address_space(target);

	cr_assert(vmm_alloc(caller_space, &params, &send_id, &send_base), "failed to allocate send buffer");
	cr_assert_eq(address_space_copy_to(caller_space, (uintptr_t)send_base, payload, sizeof(payload)),
	             ADDRESS_TRANSFER_OK);

	sched_set_current(cpu_current(), &caller_thread->thread);
	result =
		syscall_dispatch(SYSCALL_SEND_MESSAGE, process_pid(target), (uintptr_t)send_base, sizeof(payload), 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(result.value, 0u);

	cr_assert(vmm_alloc(target_space, &params, &recv_id, &recv_base), "failed to allocate recv buffer");
	cr_assert(vmm_alloc(target_space, &params, &len_id, &len_base), "failed to allocate length buffer");
	cr_assert(vmm_alloc(target_space, &params, &sender_id, &sender_base), "failed to allocate sender buffer");
	cr_assert_eq(address_space_write_uintptr(target_space, (uintptr_t)len_base, 0u), ADDRESS_TRANSFER_OK);
	cr_assert_eq(address_space_write_uintptr(target_space, (uintptr_t)sender_base, sender_value), ADDRESS_TRANSFER_OK);

	sched_set_current(cpu_current(), &target_thread->thread);
	result = syscall_dispatch(
		SYSCALL_RECV_MESSAGE, (uintptr_t)recv_base, (uintptr_t)len_base, 4u, (uintptr_t)sender_base, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(result.value, 2u);
	cr_assert_eq(address_space_read_uintptr(target_space, (uintptr_t)len_base, &length_value), ADDRESS_TRANSFER_OK);
	cr_assert_eq(length_value, sizeof(payload));
	cr_assert_eq(address_space_read_uintptr(target_space, (uintptr_t)sender_base, &sender_value), ADDRESS_TRANSFER_OK);
	cr_assert_eq(sender_value, 55u, "too-small recv should not update sender");

	result = syscall_dispatch(SYSCALL_RECV_MESSAGE,
	                          (uintptr_t)recv_base,
	                          (uintptr_t)len_base,
	                          MESSAGE_MAX_SIZE,
	                          (uintptr_t)sender_base,
	                          0u,
	                          0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(result.value, 1u);
	cr_assert_eq(address_space_read_uintptr(target_space, (uintptr_t)len_base, &length_value), ADDRESS_TRANSFER_OK);
	cr_assert_eq(length_value, sizeof(payload));
	cr_assert_eq(address_space_read_uintptr(target_space, (uintptr_t)sender_base, &sender_value), ADDRESS_TRANSFER_OK);
	cr_assert_eq(sender_value, (uintptr_t)process_pid(caller));
	cr_assert_eq(address_space_copy_from(target_space, (uintptr_t)recv_base, received, sizeof(received)),
	             ADDRESS_TRANSFER_OK);
	cr_assert_eq(memcmp(received, payload, sizeof(payload)), 0);

	thread_mark_zombie(&target_thread->thread);
	thread_mark_zombie(&caller_thread->thread);
	cr_assert(process_destroy(target), "target process_destroy failed");
	cr_assert(process_destroy(caller), "caller process_destroy failed");
	syscall_test_reset_state();
}

Test(syscall, message_send_reports_queue_full) {
	struct process*         caller;
	struct process*         target;
	struct uthread*         caller_thread;
	struct uthread*         target_thread;
	struct address_space*   caller_space;
	struct vmm_alloc_params params = {
		.page_count  = 1u,
		.align_pages = 1u,
		.prot        = VMM_PROT_READ | VMM_PROT_WRITE | VMM_PROT_USER,
		.kind        = VMM_KIND_GENERIC,
		.map_flags   = VMM_MAP_LAZY,
	};
	vmm_id_t         send_id   = VMM_ID_INVALID;
	void*            send_base = NULL;
	uint8_t          byte      = 0x5a;
	syscall_result_t result;

	syscall_test_init_process_environment();
	caller        = syscall_test_spawn_process("syscall/message-full-caller");
	target        = syscall_test_spawn_process("syscall/message-full-target");
	caller_thread = process_main_thread(caller);
	target_thread = process_main_thread(target);
	cr_assert_not_null(caller_thread);
	cr_assert_not_null(target_thread);
	caller_space = process_address_space(caller);

	cr_assert(vmm_alloc(caller_space, &params, &send_id, &send_base), "failed to allocate send buffer");
	cr_assert_eq(address_space_copy_to(caller_space, (uintptr_t)send_base, &byte, sizeof(byte)), ADDRESS_TRANSFER_OK);

	sched_set_current(cpu_current(), &caller_thread->thread);
	for (size_t i = 0u; i < MESSAGE_QUEUE_DEPTH; i++) {
		result =
			syscall_dispatch(SYSCALL_SEND_MESSAGE, process_pid(target), (uintptr_t)send_base, sizeof(byte), 0u, 0u, 0u);
		cr_assert_eq(result.status, SYSCALL_STATUS_OK);
		cr_assert_eq(result.value, 0u);
	}

	result =
		syscall_dispatch(SYSCALL_SEND_MESSAGE, process_pid(target), (uintptr_t)send_base, sizeof(byte), 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_FAILED);
	cr_assert_eq(result.value, MESSAGE_QUEUE_FULL);

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
