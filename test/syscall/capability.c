#include "test_support.h"

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

	object = cap_object_create_kernel(1u, syscall_test_cap_handler, NULL);
	cr_assert_not_null(object);
	capability = cap_create(object->cap_object_id, process_pid(process), CAP_CALL, NULL, NULL);
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
