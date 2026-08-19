#include <base/cap.h>
#include <base/channel.h>
#include <base/process.h>
#include <base/syscall.h>
#include <core/capability.h>
#include <core/channel.h>
#include <core/cpu.h>
#include <core/process.h>
#include <core/sched.h>
#include <core/thread.h>
#include <core/uthread.h>
#include <criterion/criterion.h>
#include <stdint.h>

#include "test_support.h"

static uintptr_t invalid_cap_output_pointer(void) {
	return UINTPTR_MAX - 3u;
}

static size_t cap_call_side_effect_count;

static syscall_result_t side_effecting_cap_handler(const struct cap_request* request) {
	const uint32_t response = 0x12345678u;

	cap_call_side_effect_count++;
	if (request == NULL || request->response == NULL || request->response_capacity < sizeof(response)) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	memcpy(request->response, &response, sizeof(response));
	return syscall_result_ok(sizeof(response));
}

static struct process* make_current_process(const char* name) {
	struct process* process;
	struct uthread* main_thread;

	syscall_test_init_process_environment();
	capability_init();
	process = syscall_test_spawn_process(name);
	cr_assert_not_null(process);
	main_thread = process_main_thread(process);
	cr_assert_not_null(main_thread);
	sched_set_current(cpu_current(), &main_thread->thread);
	return process;
}

static void destroy_current_process(struct process* process) {
	struct uthread* main_thread;

	if (process == NULL) return;
	main_thread = process_main_thread(process);
	if (main_thread != NULL) thread_mark_zombie(&main_thread->thread);
	sched_set_current(cpu_current(), NULL);
	cr_assert(process_destroy(process), "failed to destroy capability syscall test process");
	syscall_test_reset_state();
}

Test(capability_syscall, publish_output_failure_rolls_back_new_capability_and_object) {
	struct process*  process   = make_current_process("cap/publish-rollback");
	struct channel*  channel   = channel_create(process_pid(process));
	const uint64_t   object_id = 0x2001u;
	size_t           objects_before;
	size_t           caps_before;
	syscall_result_t result;

	cr_assert_not_null(channel);
	objects_before = capability_object_count();
	caps_before    = capability_count();
	result         = syscall_dispatch(SYSCALL_CAP_CREATE,
                              (uintptr_t)channel->id,
                              (uintptr_t)process_pid(process),
                              (uintptr_t)object_id,
                              (uintptr_t)CAP_READ,
                              invalid_cap_output_pointer(),
                              0u);

	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(capability_count(), caps_before, "failed publish left an unreachable capability record");
	cr_assert_eq(
		capability_object_count(), objects_before, "failed publish left a newly-created routing object behind");
	cr_assert_null(cap_object_lookup(channel, object_id));

	cr_assert_eq(channel_destroy(channel, process_pid(process)), CHANNEL_OK);
	destroy_current_process(process);
}

Test(capability_syscall, publish_output_failure_preserves_preexisting_object) {
	struct process*    process   = make_current_process("cap/publish-existing");
	struct channel*    channel   = channel_create(process_pid(process));
	const uint64_t     object_id = 0x2002u;
	struct cap_object* object;
	cap_object_id_t    object_record_id;
	size_t             objects_before;
	size_t             caps_before;
	syscall_result_t   result;

	cr_assert_not_null(channel);
	object_record_id = cap_object_create(object_id, channel, NULL);
	object           = cap_object_lookup(channel, object_id);
	cr_assert_neq(object_record_id, CAP_OBJECT_ID_INVALID);
	cr_assert_not_null(object);
	objects_before = capability_object_count();
	caps_before    = capability_count();

	result = syscall_dispatch(SYSCALL_CAP_CREATE,
	                          (uintptr_t)channel->id,
	                          (uintptr_t)process_pid(process),
	                          (uintptr_t)object_id,
	                          (uintptr_t)CAP_READ,
	                          invalid_cap_output_pointer(),
	                          0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(capability_count(), caps_before, "failed publish leaked a capability for a preexisting object");
	cr_assert_eq(capability_object_count(), objects_before, "rollback must preserve preexisting objects");
	cr_assert_eq(cap_object_lookup(channel, object_id), object);

	cr_assert(cap_object_destroy(object));
	cr_assert_eq(channel_destroy(channel, process_pid(process)), CHANNEL_OK);
	destroy_current_process(process);
}

Test(capability_syscall, delegate_output_failure_rolls_back_child_capability) {
	struct process*  process = make_current_process("cap/delegate-rollback");
	cap_object_id_t  object_id;
	cap_id_t         source_id;
	size_t           caps_before;
	syscall_result_t result;

	object_id = cap_object_create(0x2003u, NULL, NULL);
	cr_assert_neq(object_id, CAP_OBJECT_ID_INVALID);
	source_id = cap_create(object_id, process_pid(process), CAP_READ | CAP_DELEGATE, NULL, NULL);
	cr_assert_neq(source_id, CAP_ID_INVALID);
	caps_before = capability_count();

	result = syscall_dispatch(SYSCALL_CAP_DELEGATE,
	                          (uintptr_t)source_id,
	                          (uintptr_t)process_pid(process),
	                          (uintptr_t)CAP_READ,
	                          invalid_cap_output_pointer(),
	                          0u,
	                          0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(capability_count(), caps_before, "failed delegation left an unreachable child capability");

	cr_assert(cap_destroy_by_id(source_id));
	cr_assert(cap_object_destroy_with_id(object_id));
	destroy_current_process(process);
}

Test(capability_syscall, derive_output_failure_rolls_back_child_and_new_object) {
	struct process*  process = make_current_process("cap/derive-rollback");
	struct channel*  channel = channel_create(process_pid(process));
	cap_object_id_t  base_object_id;
	cap_id_t         base_cap_id;
	const uint64_t   derived_object_id = 0x2005u;
	size_t           objects_before;
	size_t           caps_before;
	syscall_result_t result;

	cr_assert_not_null(channel);
	base_object_id = cap_object_create(0x2004u, channel, NULL);
	cr_assert_neq(base_object_id, CAP_OBJECT_ID_INVALID);
	base_cap_id = cap_create(base_object_id, process_pid(process), CAP_DERIVE, NULL, NULL);
	cr_assert_neq(base_cap_id, CAP_ID_INVALID);
	objects_before = capability_object_count();
	caps_before    = capability_count();

	result = syscall_dispatch(SYSCALL_CAP_DERIVE,
	                          (uintptr_t)base_cap_id,
	                          (uintptr_t)process_pid(process),
	                          (uintptr_t)derived_object_id,
	                          (uintptr_t)CAP_READ,
	                          invalid_cap_output_pointer(),
	                          0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(capability_count(), caps_before, "failed derive left an unreachable child capability");
	cr_assert_eq(
		capability_object_count(), objects_before, "failed derive left its newly-created derived object behind");
	cr_assert_null(cap_object_lookup(channel, derived_object_id));

	cr_assert(cap_destroy_by_id(base_cap_id));
	cr_assert(cap_object_destroy_with_id(base_object_id));
	cr_assert_eq(channel_destroy(channel, process_pid(process)), CHANNEL_OK);
	destroy_current_process(process);
}

Test(capability_syscall, call_validates_response_before_handler_side_effects) {
	struct process*               process        = make_current_process("cap/call-response-validation");
	const struct vmm_alloc_params request_params = {
		.page_count  = 1u,
		.align_pages = 1u,
		.prot        = VMM_PROT_READ | VMM_PROT_WRITE | VMM_PROT_USER,
		.kind        = VMM_KIND_GENERIC,
	};
	const struct vmm_alloc_params response_params = {
		.page_count  = 1u,
		.align_pages = 1u,
		.prot        = VMM_PROT_READ | VMM_PROT_WRITE | VMM_PROT_USER,
		.kind        = VMM_KIND_GENERIC,
		.map_flags   = VMM_MAP_LAZY,
	};
	cap_object_id_t  object_id;
	cap_id_t         capability_id;
	vmm_id_t         request_id      = VMM_ID_INVALID;
	vmm_id_t         response_id     = VMM_ID_INVALID;
	void*            request_buffer  = NULL;
	void*            response_buffer = NULL;
	uint32_t         response_value  = 0u;
	syscall_result_t result;

	cr_assert(vmm_alloc(process_address_space(process), &request_params, &request_id, &request_buffer));
	cr_assert_not_null(request_buffer);
	cr_assert(vmm_alloc(process_address_space(process), &response_params, &response_id, &response_buffer));
	cr_assert_not_null(response_buffer);

	object_id = cap_object_create_kernel(0x2006u, side_effecting_cap_handler, NULL);
	cr_assert_neq(object_id, CAP_OBJECT_ID_INVALID);
	capability_id = cap_create(object_id, process_pid(process), CAP_CALL, NULL, NULL);
	cr_assert_neq(capability_id, CAP_ID_INVALID);

	cap_call_side_effect_count = 0u;
	result                     = syscall_dispatch(SYSCALL_CAP_CALL,
                              (uintptr_t)capability_id,
                              (uintptr_t)request_buffer,
                              1u,
                              invalid_cap_output_pointer(),
                              sizeof(response_value),
                              0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(result.value, 3u, "invalid response buffer should be reported as the response argument");
	cr_assert_eq(cap_call_side_effect_count, 0u, "handler ran before the response buffer was validated");

	result = syscall_dispatch(SYSCALL_CAP_CALL,
	                          (uintptr_t)capability_id,
	                          (uintptr_t)request_buffer,
	                          1u,
	                          (uintptr_t)response_buffer,
	                          sizeof(response_value),
	                          0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(result.value, sizeof(response_value));
	cr_assert_eq(cap_call_side_effect_count, 1u, "valid call should execute the handler exactly once");
	cr_assert_eq(
		address_space_copy_from(
			process_address_space(process), (uintptr_t)response_buffer, &response_value, sizeof(response_value)),
		ADDRESS_TRANSFER_OK);
	cr_assert_eq(response_value, 0x12345678u, "validated lazy response buffer did not receive the handler response");

	cr_assert(cap_destroy_by_id(capability_id));
	cr_assert(cap_object_destroy_with_id(object_id));
	cr_assert(vmm_free(process_address_space(process), response_id));
	cr_assert(vmm_free(process_address_space(process), request_id));
	destroy_current_process(process);
}
