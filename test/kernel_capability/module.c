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
	const struct {
		struct module_provider_resolve_request request;
		char                                   name[sizeof("sample.elf")];
	} request = {
		.request = {.header = {.op = MODULE_PROVIDER_OP_RESOLVE}, .name_size = sizeof("sample.elf")},
		.name    = "sample.elf",
	};
	struct module_provider_resolve_response response;
	cap_id_t                                provider_cap;
	cap_id_t                                original_cap;
	struct capability*                      retained;
	syscall_result_t                        result;

	kernel_capability_test_begin(&ctx, "kernel-cap/module-rollback");
	kernel_boot_mock_set_modules(modules, 1u);
	kernel_capability_boot_module_provider_init();
	provider_cap = kernel_capability_boot_module_provider_grant(process_pid(ctx.process));
	cr_assert_neq(provider_cap, CAP_ID_INVALID);

	result = kernel_capability_test_call(
		provider_cap, &request, sizeof(request.request) + sizeof(request.name), &response, sizeof(response));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	original_cap = response.cap;
	cr_assert_neq(original_cap, CAP_ID_INVALID);
	retained = cap_acquire(original_cap);
	cr_assert_not_null(retained);
	cap_release(retained);

	result = kernel_capability_test_call(
		provider_cap, &request, sizeof(request.request) + sizeof(request.name), &response, sizeof(response) - 1u);
	cr_assert_neq(result.status, SYSCALL_STATUS_OK);
	retained = cap_acquire(original_cap);
	cr_assert_not_null(retained,
	                   "failed repeat module resolve destroyed a capability that existed before the failed call");
	cap_release(retained);

	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_module, provider_resolves_paths_and_rejects_unterminated_names) {
	struct kernel_capability_test_context ctx;
	static const uint8_t                  module_bytes[] = {0x41u};
	const struct kernel_boot_module       modules[]      = {
        {.name = "path.bin", .path = "/boot/path.bin", .address = (void*)module_bytes, .size = sizeof(module_bytes)},
    };
	const struct {
		struct module_provider_resolve_request request;
		char                                   name[sizeof("/boot/path.bin")];
	} path_request = {
		.request = {.header = {.op = MODULE_PROVIDER_OP_RESOLVE}, .name_size = sizeof("/boot/path.bin")},
		.name    = "/boot/path.bin",
	};
	const struct {
		struct module_provider_resolve_request request;
		char                                   name[3];
	} bad_request = {
		.request = {.header = {.op = MODULE_PROVIDER_OP_RESOLVE}, .name_size = 3u},
		.name    = {'b', 'a', 'd'},
	};
	struct module_provider_resolve_response response;
	cap_id_t                                provider_cap;
	syscall_result_t                        result;

	kernel_capability_test_begin(&ctx, "kernel-cap/module-provider-validation");
	kernel_boot_mock_set_modules(modules, 1u);
	kernel_capability_boot_module_provider_init();
	provider_cap = kernel_capability_boot_module_provider_grant(process_pid(ctx.process));
	cr_assert_neq(provider_cap, CAP_ID_INVALID);
	result = kernel_capability_test_call(provider_cap,
	                                     &path_request,
	                                     sizeof(path_request.request) + sizeof(path_request.name),
	                                     &response,
	                                     sizeof(response));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(response.id, 1u);
	cr_assert_str_eq(response.path, "/boot/path.bin");
	result = kernel_capability_test_call(provider_cap,
	                                     &bad_request,
	                                     sizeof(bad_request.request) + sizeof(bad_request.name),
	                                     &response,
	                                     sizeof(response));
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
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

Test(kernel_capability_module, provider_and_module_capabilities_use_distinct_protocols) {
	struct kernel_capability_test_context ctx;
	static const uint8_t                  module_bytes[] = {0x21u};
	const struct kernel_boot_module       modules[]      = {
        {.name = "distinct.bin", .address = (void*)module_bytes, .size = sizeof(module_bytes)},
    };
	const struct module_info_request info_request = {.header = {.op = MODULE_OP_INFO}};
	struct module_info_response      info_response;
	const struct kernel_boot_module* resolved = NULL;
	struct capability*               provider;
	struct capability*               module;
	cap_id_t                         provider_cap;
	cap_id_t                         module_cap;
	syscall_result_t                 result;

	kernel_capability_test_begin(&ctx, "kernel-cap/module-provider-distinct");
	kernel_boot_mock_set_modules(modules, 1u);
	kernel_capability_boot_module_provider_init();
	provider_cap = kernel_capability_boot_module_provider_grant(process_pid(ctx.process));
	module_cap   = kernel_capability_boot_module_grant(0u, process_pid(ctx.process));
	cr_assert_neq(provider_cap, CAP_ID_INVALID);
	cr_assert_neq(module_cap, CAP_ID_INVALID);
	provider = cap_acquire(provider_cap);
	module   = cap_acquire(module_cap);
	cr_assert_not_null(provider);
	cr_assert_not_null(module);
	cr_assert_neq(provider->cap_object_id, module->cap_object_id);
	cap_release(provider);
	cap_release(module);

	result = kernel_capability_test_call(
		provider_cap, &info_request, sizeof(info_request), &info_response, sizeof(info_response));
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	result = kernel_capability_boot_module_get(provider_cap, process_pid(ctx.process), CAP_READ, &resolved);
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_null(resolved);
	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_module, direct_resolution_uses_read_right_without_cap_call) {
	struct kernel_capability_test_context ctx;
	static const uint8_t                  module_bytes[] = {0x33u};
	const struct kernel_boot_module       modules[]      = {
        {
         .name    = "direct.bin",
         .path    = "/boot/direct.bin",
         .address = (void*)module_bytes,
         .size    = sizeof(module_bytes),
         },
    };
	const struct kernel_boot_module* resolved = NULL;
	struct capability*               original;
	cap_id_t                         original_id;
	cap_id_t                         read_only_id;

	kernel_capability_test_begin(&ctx, "kernel-cap/module-direct-resolution");
	kernel_boot_mock_set_modules(modules, 1u);
	original_id = kernel_capability_boot_module_grant(0u, process_pid(ctx.process));
	original    = cap_acquire(original_id);
	cr_assert_not_null(original);
	read_only_id = cap_create(original->cap_object_id, process_pid(ctx.process), CAP_READ, NULL);
	cap_release(original);
	cr_assert_neq(read_only_id, CAP_ID_INVALID);
	cr_assert_eq(kernel_capability_boot_module_get(read_only_id, process_pid(ctx.process), CAP_READ, &resolved).status,
	             SYSCALL_STATUS_OK);
	cr_assert_not_null(resolved);
	cr_assert_eq(resolved->address, modules[0].address);
	cr_assert_eq(resolved->size, modules[0].size);
	kernel_capability_test_end(&ctx);
}
