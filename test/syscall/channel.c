#include <core/signal.h>

#include "test_support.h"

Test(syscall, channel_create_and_destroy_manage_process_owned_state) {
	struct process*       process;
	struct uthread*       main_thread;
	struct address_space* space;
	struct channel*       channel;
	vmm_id_t              output_id = VMM_ID_INVALID;
	void*                 output_base;
	channel_id_t          channel_id = CHANNEL_ID_INVALID;
	syscall_result_t      result;

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

	cr_assert(test_vm_map(space, 1u, VMM_PROT_READ | VMM_PROT_WRITE, 0u, 1u, 0u, &output_id, &output_base),
	          "failed to allocate channel ID output");
	result = syscall_dispatch(SYSCALL_CHANNEL_CREATE, (uintptr_t)output_base, 0u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(address_space_copy_from(space, (uintptr_t)output_base, &channel_id, sizeof(channel_id)),
	             ADDRESS_TRANSFER_OK);
	cr_assert_neq(channel_id, CHANNEL_ID_INVALID);
	cr_assert_eq(process->channel_state.count, 1u);
	channel = channel_acquire(channel_id);
	cr_assert_not_null(channel);
	cr_assert_eq(channel->owner_pid, process_pid(process));
	cr_assert_null(channel_activity_signal(channel));
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

Test(syscall, channel_create_optionally_returns_an_activity_signal_capability) {
	struct process*       process;
	struct uthread*       main_thread;
	struct address_space* space;
	struct capability*    activity_grant;
	struct channel*       channel;
	vmm_id_t              output_id = VMM_ID_INVALID;
	void*                 output_base;
	channel_id_t          channel_id   = CHANNEL_ID_INVALID;
	cap_id_t              activity_cap = CAP_ID_INVALID;
	size_t                caps_before;
	size_t                signals_before;
	syscall_result_t      result;

	syscall_test_init_process_environment();
	capability_init();
	process     = syscall_test_spawn_process("syscall/channel-activity");
	main_thread = process_main_thread(process);
	cr_assert_not_null(main_thread);
	sched_set_current(cpu_current(), &main_thread->thread);
	space          = process_address_space(process);
	caps_before    = capability_count();
	signals_before = signal_count();

	cr_assert(test_vm_map(space, 1u, VMM_PROT_READ | VMM_PROT_WRITE, 0u, 1u, 0u, &output_id, &output_base));
	result = syscall_dispatch(
		SYSCALL_CHANNEL_CREATE, (uintptr_t)output_base, MM_USER_VMM_BASE + MM_USER_VMM_SIZE, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(result.value, 1u);
	cr_assert_eq(process->channel_state.count, 0u);
	cr_assert_eq(capability_count(), caps_before);
	cr_assert_eq(signal_count(), signals_before);

	result = syscall_dispatch(
		SYSCALL_CHANNEL_CREATE, (uintptr_t)output_base, (uintptr_t)output_base + sizeof(channel_id), 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(address_space_copy_from(space, (uintptr_t)output_base, &channel_id, sizeof(channel_id)),
	             ADDRESS_TRANSFER_OK);
	cr_assert_eq(address_space_copy_from(
					 space, (uintptr_t)output_base + sizeof(channel_id), &activity_cap, sizeof(activity_cap)),
	             ADDRESS_TRANSFER_OK);
	cr_assert_neq(channel_id, CHANNEL_ID_INVALID);
	cr_assert_neq(activity_cap, CAP_ID_INVALID);
	cr_assert_eq(process->channel_state.count, 1u);
	cr_assert_eq(capability_count(), caps_before + 1u);
	cr_assert_eq(signal_count(), signals_before + 1u);
	activity_grant = cap_acquire(activity_cap);
	cr_assert_not_null(activity_grant);
	cr_assert_eq(activity_grant->target, process_pid(process));
	cr_assert_eq(cap_rights(activity_grant), CAP_READ | CAP_WAIT);
	cap_release(activity_grant);
	channel = channel_acquire(channel_id);
	cr_assert_not_null(channel);
	cr_assert_not_null(channel_activity_signal(channel));
	channel_release(channel);

	result = syscall_dispatch(SYSCALL_CHANNEL_DESTROY, channel_id, 0u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(capability_count(), caps_before);
	cr_assert_eq(signal_count(), signals_before);

	thread_mark_zombie(&main_thread->thread);
	cr_assert(process_destroy(process));
	syscall_test_reset_state();
}

Test(syscall, cap_unpublish_requires_endpoint_owner_authority) {
	struct process*  owner;
	struct process*  other;
	struct uthread*  owner_thread;
	struct uthread*  other_thread;
	struct channel*  endpoint;
	cap_object_id_t  object_id;
	syscall_result_t result;

	syscall_test_init_process_environment();
	capability_init();
	owner        = syscall_test_spawn_process("syscall/unpublish-owner");
	other        = syscall_test_spawn_process("syscall/unpublish-other");
	owner_thread = process_main_thread(owner);
	other_thread = process_main_thread(other);
	endpoint     = channel_create(process_pid(owner), false);
	cr_assert_not_null(endpoint);
	object_id = cap_object_create(0x500u, endpoint, NULL);
	cr_assert_neq(object_id, CAP_OBJECT_ID_INVALID);
	cr_assert_neq(cap_create(object_id, process_pid(other), CAP_CALL, NULL), CAP_ID_INVALID);

	sched_set_current(cpu_current(), &other_thread->thread);
	result = syscall_dispatch(SYSCALL_CAP_UNPUBLISH, endpoint->id, 0x500u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_DENIED);
	struct cap_object* retained = cap_object_acquire(object_id);
	cr_assert_not_null(retained);
	cap_object_release(retained);

	sched_set_current(cpu_current(), &owner_thread->thread);
	result = syscall_dispatch(SYSCALL_CAP_UNPUBLISH, endpoint->id, 0x500u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_null(cap_object_acquire(object_id));
	cr_assert_eq(channel_destroy(endpoint, process_pid(owner)), CHANNEL_OK);

	thread_mark_zombie(&other_thread->thread);
	cr_assert(process_destroy(other));
	sched_set_current(cpu_current(), &owner_thread->thread);
	thread_mark_zombie(&owner_thread->thread);
	cr_assert(process_destroy(owner));
	syscall_test_reset_state();
}

Test(syscall, cap_unpublish_if_unused_preserves_new_grants) {
	struct process*    owner;
	struct process*    other;
	struct uthread*    owner_thread;
	struct uthread*    other_thread;
	struct channel*    endpoint;
	struct capability* grant;
	cap_object_id_t    object_id;
	syscall_result_t   result;

	syscall_test_init_process_environment();
	capability_init();
	owner        = syscall_test_spawn_process("syscall/unused-owner");
	other        = syscall_test_spawn_process("syscall/unused-other");
	owner_thread = process_main_thread(owner);
	other_thread = process_main_thread(other);
	endpoint     = channel_create(process_pid(owner), false);
	cr_assert_not_null(endpoint);
	object_id = cap_object_create(0x501u, endpoint, NULL);
	cr_assert_neq(object_id, CAP_OBJECT_ID_INVALID);
	grant = cap_acquire(cap_create(object_id, process_pid(other), CAP_READ, NULL));
	cr_assert_not_null(grant);

	sched_set_current(cpu_current(), &other_thread->thread);
	result = syscall_dispatch(SYSCALL_CAP_UNPUBLISH, endpoint->id, 0x501u, 1u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_DENIED);
	cr_assert_eq(cap_is_valid(grant), CAP_OK);

	sched_set_current(cpu_current(), &owner_thread->thread);
	result = syscall_dispatch(SYSCALL_CAP_UNPUBLISH, endpoint->id, 0x501u, 1u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_UNAVAILABLE);
	cr_assert_eq(cap_is_valid(grant), CAP_OK);
	cr_assert(cap_destroy(grant));
	cap_release(grant);
	result = syscall_dispatch(SYSCALL_CAP_UNPUBLISH, endpoint->id, 0x501u, 1u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_null(cap_object_acquire(object_id));
	cr_assert_eq(channel_destroy(endpoint, process_pid(owner)), CHANNEL_OK);

	thread_mark_zombie(&other_thread->thread);
	cr_assert(process_destroy(other));
	sched_set_current(cpu_current(), &owner_thread->thread);
	thread_mark_zombie(&owner_thread->thread);
	cr_assert(process_destroy(owner));
	syscall_test_reset_state();
}
