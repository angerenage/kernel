#include "test_support.h"

Test(syscall, capability_call_uses_distinct_request_and_response_buffers) {
	struct process*                  process;
	struct uthread*                  main_thread;
	cap_object_id_t                  object_id;
	cap_id_t                         capability_id;
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

	object_id = cap_object_create_kernel(1u, syscall_test_cap_handler, NULL);
	cr_assert_neq(object_id, CAP_OBJECT_ID_INVALID);
	capability_id = cap_create(object_id, process_pid(process), CAP_CALL, NULL, NULL);
	cr_assert_neq(capability_id, CAP_ID_INVALID);

	result = syscall_dispatch(SYSCALL_CAP_CALL,
	                          capability_id,
	                          (uintptr_t)&request,
	                          sizeof(request),
	                          (uintptr_t)&response,
	                          sizeof(response),
	                          0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(result.value, sizeof(response));
	cr_assert_eq(response.value, 42u);

	result = syscall_dispatch(SYSCALL_CAP_CALL, capability_id, 0u, 0u, (uintptr_t)&response, sizeof(response), 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(result.value, 2u);

	result = syscall_dispatch(
		SYSCALL_CAP_CALL, capability_id, (uintptr_t)&request, sizeof(request), 0u, sizeof(response), 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(result.value, 3u);

	cr_assert(cap_destroy_by_id(capability_id));
	cr_assert(cap_object_destroy_with_id(object_id));
	syscall_test_reset_state();
}

Test(syscall, capability_valid_reports_current_validity_without_disclosing_foreign_caps) {
	struct process*    owner;
	struct process*    other;
	struct uthread*    owner_thread;
	struct uthread*    other_thread;
	cap_object_id_t    object_id;
	cap_id_t           capability_id;
	syscall_result_t   result;
	struct capability* capability;

	syscall_test_init_process_environment();
	capability_init();
	owner        = syscall_test_spawn_process("syscall/cap-valid-owner");
	other        = syscall_test_spawn_process("syscall/cap-valid-other");
	owner_thread = process_main_thread(owner);
	other_thread = process_main_thread(other);
	cr_assert_not_null(owner_thread);
	cr_assert_not_null(other_thread);
	owner_thread->thread.address_space = NULL;
	other_thread->thread.address_space = NULL;

	object_id = cap_object_create_kernel(2u, syscall_test_cap_handler, NULL);
	cr_assert_neq(object_id, CAP_OBJECT_ID_INVALID);
	capability_id = cap_create(object_id, process_pid(owner), CAP_CALL, NULL, NULL);
	cr_assert_neq(capability_id, CAP_ID_INVALID);

	sched_set_current(cpu_current(), &owner_thread->thread);
	result = syscall_dispatch(SYSCALL_CAP_VALID, capability_id, 0u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(result.value, 1u);

	sched_set_current(cpu_current(), &other_thread->thread);
	result = syscall_dispatch(SYSCALL_CAP_VALID, capability_id, 0u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(result.value, 0u);

	sched_set_current(cpu_current(), &owner_thread->thread);
	capability = cap_acquire(capability_id);
	cr_assert_not_null(capability);
	cr_assert(cap_destroy(capability));
	cap_release(capability);
	result = syscall_dispatch(SYSCALL_CAP_VALID, capability_id, 0u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(result.value, 0u);

	cr_assert_not(cap_destroy_by_id(capability_id));
	result = syscall_dispatch(SYSCALL_CAP_VALID, capability_id, 0u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(result.value, 0u);
	cr_assert(cap_object_destroy_with_id(object_id));
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

Test(syscall, capability_drop_only_allows_direct_target) {
	struct process*  owner;
	struct process*  other;
	struct uthread*  owner_thread;
	struct uthread*  other_thread;
	cap_object_id_t  object_id;
	cap_id_t         capability_id;
	syscall_result_t result;

	syscall_test_init_process_environment();
	capability_init();
	owner        = syscall_test_spawn_process("syscall/cap-drop-owner");
	other        = syscall_test_spawn_process("syscall/cap-drop-other");
	owner_thread = process_main_thread(owner);
	other_thread = process_main_thread(other);
	cr_assert_not_null(owner_thread);
	cr_assert_not_null(other_thread);
	owner_thread->thread.address_space = NULL;
	other_thread->thread.address_space = NULL;

	object_id = cap_object_create_kernel(3u, syscall_test_cap_handler, NULL);
	cr_assert_neq(object_id, CAP_OBJECT_ID_INVALID);
	capability_id = cap_create(object_id, process_pid(owner), CAP_CALL, NULL, NULL);
	cr_assert_neq(capability_id, CAP_ID_INVALID);

	sched_set_current(cpu_current(), &other_thread->thread);
	result = syscall_dispatch(SYSCALL_CAP_DROP, capability_id, 0u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_not_null(cap_lookup(capability_id), "a foreign process must not be able to drop the capability");

	sched_set_current(cpu_current(), &owner_thread->thread);
	result = syscall_dispatch(SYSCALL_CAP_DROP, capability_id, 0u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_null(cap_lookup(capability_id));

	cr_assert(cap_object_destroy_with_id(object_id));
	syscall_test_reset_state();
}

Test(syscall, capability_delegate_peer_uses_source_parent) {
	struct process*    process;
	struct uthread*    main_thread;
	struct capability* root;
	struct capability* source;
	struct capability* normal;
	struct capability* peer;
	cap_object_id_t    object_id;
	cap_id_t           root_id;
	cap_id_t           source_id;
	cap_id_t           normal_id = CAP_ID_INVALID;
	cap_id_t           peer_id   = CAP_ID_INVALID;
	syscall_result_t   result;
	process_id_t       pid;

	syscall_test_init_process_environment();
	capability_init();
	process     = syscall_test_spawn_process("syscall/cap-delegate-peer");
	main_thread = process_main_thread(process);
	cr_assert_not_null(main_thread);
	main_thread->thread.address_space = NULL;
	sched_set_current(cpu_current(), &main_thread->thread);
	pid = process_pid(process);

	object_id = cap_object_create_kernel(4u, syscall_test_cap_handler, NULL);
	cr_assert_neq(object_id, CAP_OBJECT_ID_INVALID);
	root_id   = cap_create(object_id, pid, CAP_READ | CAP_DELEGATE | CAP_DELEGATE_PEER, NULL, NULL);
	root      = cap_lookup(root_id);
	source_id = cap_create(object_id, pid, CAP_READ | CAP_DELEGATE | CAP_DELEGATE_PEER, root, NULL);
	source    = cap_lookup(source_id);
	cr_assert_not_null(root);
	cr_assert_not_null(source);

	result = syscall_dispatch(
		SYSCALL_CAP_DELEGATE, source_id, pid, CAP_READ, (uintptr_t)&normal_id, (uintptr_t)CAP_DELEGATE_FLAG_NONE, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	normal = cap_lookup(normal_id);
	cr_assert_not_null(normal);
	cr_assert_eq(normal->parent, source);

	result = syscall_dispatch(
		SYSCALL_CAP_DELEGATE, source_id, pid, CAP_READ, (uintptr_t)&peer_id, (uintptr_t)CAP_DELEGATE_FLAG_PEER, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	peer = cap_lookup(peer_id);
	cr_assert_not_null(peer);
	cr_assert_eq(peer->parent, root);

	result = syscall_dispatch(SYSCALL_CAP_DELEGATE,
	                          source_id,
	                          pid,
	                          CAP_READ,
	                          (uintptr_t)&peer_id,
	                          (uintptr_t)(CAP_DELEGATE_FLAG_PEER | (1u << 1)),
	                          0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(result.value, 4u);

	cr_assert(cap_destroy(source));
	cr_assert_null(cap_lookup(source_id));
	cr_assert_null(cap_lookup(normal_id));
	cr_assert_eq(cap_lookup(peer_id), peer);
	cr_assert_eq(cap_is_valid(peer), CAP_OK);

	cr_assert(cap_destroy(root));
	cr_assert_null(cap_lookup(peer_id));
	cr_assert(cap_object_destroy_with_id(object_id));
	syscall_test_reset_state();
}
