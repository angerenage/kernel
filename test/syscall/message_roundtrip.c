#include "test_support.h"

Test(syscall, message_send_and_recv_roundtrip) {
	struct process*       caller;
	struct process*       target;
	struct uthread*       caller_thread;
	struct uthread*       target_thread;
	struct address_space* caller_space;
	struct address_space* target_space;
	vmm_id_t              send_id     = VMM_ID_INVALID;
	vmm_id_t              recv_id     = VMM_ID_INVALID;
	vmm_id_t              len_id      = VMM_ID_INVALID;
	vmm_id_t              sender_id   = VMM_ID_INVALID;
	void*                 send_base   = NULL;
	void*                 recv_base   = NULL;
	void*                 len_base    = NULL;
	void*                 sender_base = NULL;
	const char            payload[]   = "syscall-message";
	char                  received[sizeof(payload)];
	uintptr_t             length_value = 0u;
	uintptr_t             sender_value = 0u;
	syscall_result_t      result;

	syscall_test_init_process_environment();
	caller        = syscall_test_spawn_process("syscall/message-caller");
	target        = syscall_test_spawn_process("syscall/message-target");
	caller_thread = process_main_thread(caller);
	target_thread = process_main_thread(target);
	cr_assert_not_null(caller_thread);
	cr_assert_not_null(target_thread);
	caller_space = process_address_space(caller);
	target_space = process_address_space(target);

	cr_assert(test_vm_map(caller_space, 1u, VMM_PROT_READ | VMM_PROT_WRITE, 0u, 1u, 0u, &send_id, &send_base),
	          "failed to allocate send buffer");
	cr_assert_eq(address_space_copy_to(caller_space, (uintptr_t)send_base, payload, sizeof(payload)),
	             ADDRESS_TRANSFER_OK);

	sched_set_current(cpu_current(), &caller_thread->thread);
	result =
		syscall_dispatch(SYSCALL_SEND_MESSAGE, process_pid(target), (uintptr_t)send_base, sizeof(payload), 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(result.value, 0u);

	cr_assert(test_vm_map(target_space, 1u, VMM_PROT_READ | VMM_PROT_WRITE, 0u, 1u, 0u, &recv_id, &recv_base),
	          "failed to allocate recv buffer");
	cr_assert(test_vm_map(target_space, 1u, VMM_PROT_READ | VMM_PROT_WRITE, 0u, 1u, 0u, &len_id, &len_base),
	          "failed to allocate length buffer");
	cr_assert(test_vm_map(target_space, 1u, VMM_PROT_READ | VMM_PROT_WRITE, 0u, 1u, 0u, &sender_id, &sender_base),
	          "failed to allocate sender buffer");
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
