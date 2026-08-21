#include "../../kernel/src/syscall/module.h"

#include "../../kernel/src/capability/boot_module.h"
#include "test_support.h"

Test(kernel_capability_module, failed_repeat_resolve_preserves_the_preexisting_module_capability) {
	struct kernel_capability_test_context ctx;
	static const uint8_t                  module_bytes[] = {1u, 2u, 3u, 4u};
	const struct kernel_boot_module       modules[]      = {
        {
         .name       = "sample.elf",
         .path       = "/boot/sample.elf",
         .address    = (void*)module_bytes,
         .size       = sizeof(module_bytes),
         .media_type = 0u,
         },
    };
	const char                   name[] = "sample.elf";
	struct module_query_response response;
	vmm_id_t                     out_id = VMM_ID_INVALID;
	uintptr_t                    out_address;
	cap_id_t                     original_cap;
	struct capability*           retained;
	syscall_result_t             result;

	kernel_capability_test_begin(&ctx, "kernel-cap/module-rollback");
	kernel_boot_mock_set_modules(modules, 1u);
	out_address = kernel_capability_test_alloc_user_buffer(ctx.process, 1u, &out_id);

	result = syscall_module_resolve((uintptr_t)name, sizeof(name), out_address, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(address_space_copy_from(process_address_space(ctx.process), out_address, &response, sizeof(response)),
	             ADDRESS_TRANSFER_OK);
	original_cap = response.cap;
	cr_assert_neq(original_cap, CAP_ID_INVALID);
	retained = cap_acquire(original_cap);
	cr_assert_not_null(retained);
	cap_release(retained);

	result = syscall_module_resolve(
		(uintptr_t)name, sizeof(name), MM_USER_VMM_BASE + MM_USER_VMM_SIZE + PMM_PAGE_SIZE, 0u, 0u, 0u);
	cr_assert_neq(result.status, SYSCALL_STATUS_OK);
	retained = cap_acquire(original_cap);
	cr_assert_not_null(retained,
	                   "failed repeat module_resolve destroyed a capability that existed before the failed syscall");
	cap_release(retained);

	if (out_id != VMM_ID_INVALID) cr_assert(vm_space_unmap(process_address_space(ctx.process), out_id));
	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_module, zero_length_read_is_a_successful_noop) {
	struct kernel_capability_test_context ctx;
	static const uint8_t                  module_bytes[] = {0x11u, 0x22u};
	const struct kernel_boot_module       modules[]      = {
        {
         .name    = "bytes.bin",
         .path    = "/boot/bytes.bin",
         .address = (void*)module_bytes,
         .size    = sizeof(module_bytes),
         },
    };
	const struct module_read_request request = {
		.header = {.op = MODULE_OP_READ},
		.offset = sizeof(module_bytes),
		.size   = 0u,
	};
	cap_id_t         cap;
	syscall_result_t result;

	kernel_capability_test_begin(&ctx, "kernel-cap/module-zero-read");
	kernel_boot_mock_set_modules(modules, 1u);
	cap = kernel_capability_boot_module_grant(0u, process_pid(ctx.process));
	cr_assert_neq(cap, CAP_ID_INVALID);

	result = kernel_capability_test_call(cap, &request, sizeof(request), NULL, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK, "zero-length module read should be a successful no-op");
	cr_assert_eq(result.value, 0u);

	kernel_capability_test_end(&ctx);
}
