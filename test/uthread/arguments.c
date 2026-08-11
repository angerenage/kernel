#include "test_support.h"

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
