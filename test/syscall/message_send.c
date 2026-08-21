#include "test_support.h"

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

Test(syscall, message_send_reports_queue_full) {
	struct process*       caller;
	struct process*       target;
	struct uthread*       caller_thread;
	struct uthread*       target_thread;
	struct address_space* caller_space;
	vmm_id_t              send_id   = VMM_ID_INVALID;
	void*                 send_base = NULL;
	uint8_t               byte      = 0x5a;
	syscall_result_t      result;

	syscall_test_init_process_environment();
	caller        = syscall_test_spawn_process("syscall/message-full-caller");
	target        = syscall_test_spawn_process("syscall/message-full-target");
	caller_thread = process_main_thread(caller);
	target_thread = process_main_thread(target);
	cr_assert_not_null(caller_thread);
	cr_assert_not_null(target_thread);
	caller_space = process_address_space(caller);

	cr_assert(test_vm_map(caller_space, 1u, VMM_PROT_READ | VMM_PROT_WRITE, 0u, 1u, 0u, &send_id, &send_base),
	          "failed to allocate send buffer");
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
