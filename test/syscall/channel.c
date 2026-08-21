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
