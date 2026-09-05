#include "../../kernel/src/capability/memory.h"

#include <base/vmm.h>

#include "../../kernel/src/syscall/memory.h"
#include "test_support.h"

static cap_id_t create_memory_params(cap_rights_t rights, const struct memory_create_params* params) {
	return kernel_memory_create(rights, params);
}

static cap_id_t create_memory(cap_rights_t rights, size_t page_count) {
	const struct memory_create_params params = {.page_count = page_count};
	return create_memory_params(rights, &params);
}

static bool drop_capability(cap_id_t id) {
	struct capability* cap = cap_acquire(id);
	if (cap == NULL) return false;
	bool dropped = cap_drop(cap);
	cap_release(cap);
	return dropped;
}

static cap_object_id_t capability_object_id(cap_id_t id) {
	struct capability* cap = cap_acquire(id);
	if (cap == NULL) return CAP_OBJECT_ID_INVALID;
	cap_object_id_t object_id = cap->cap_object_id;
	cap_release(cap);
	return object_id;
}

static syscall_result_t memory_info(cap_id_t cap, struct memory_info* out_info) {
	const struct memory_info_request request = {.header = {.op = MEMORY_OP_INFO}};
	return kernel_capability_test_call(cap, &request, sizeof(request), out_info, sizeof(*out_info));
}

static syscall_result_t memory_read_to(cap_id_t cap, size_t offset, uintptr_t destination, size_t size) {
	const struct memory_read_request request = {
		.header = {.op = MEMORY_OP_READ}, .offset = offset, .destination = destination, .size = size};
	struct memory_transfer_response response;
	return kernel_capability_test_call(cap, &request, sizeof(request), &response, sizeof(response));
}

static syscall_result_t memory_write_from(cap_id_t cap, size_t offset, uintptr_t source, size_t size) {
	const struct memory_write_request request = {
		.header = {.op = MEMORY_OP_WRITE}, .source = source, .offset = offset, .size = size};
	struct memory_transfer_response response;
	return kernel_capability_test_call(cap, &request, sizeof(request), &response, sizeof(response));
}

static syscall_result_t map_memory(cap_id_t memory_cap, process_id_t caller, struct process* target,
                                   const struct memory_map_params* params, struct address_space_map_result* out) {
	return kernel_memory_map(memory_cap, caller, target, params, out);
}

static syscall_result_t protect_mapping(cap_id_t cap, vmm_prot_t prot) {
	const struct mapping_protect_request request = {.header = {.op = MAPPING_OP_PROTECT}, .prot = prot};
	return kernel_capability_test_call(cap, &request, sizeof(request), NULL, 0u);
}

Test(kernel_capability_memory, create_syscall_grants_the_final_memory_rights) {
	struct kernel_capability_test_context ctx;

	kernel_capability_test_begin(&ctx, "kernel-cap/memory-create-rights");
	const struct memory_create_params params = {.page_count = 1u};
	syscall_result_t result = syscall_memory_create((uintptr_t)&params, sizeof(params), 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	struct capability* cap = cap_acquire((cap_id_t)result.value);
	cr_assert_not_null(cap);
	cr_assert_eq(cap_rights(cap), CAP_CALL | CAP_READ | CAP_WRITE | CAP_EXEC | CAP_MAP | CAP_DELEGATE);
	cap_release(cap);
	cr_assert(drop_capability((cap_id_t)result.value));
	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_memory, create_reports_logical_size_and_fresh_contents_are_zero) {
	struct kernel_capability_test_context ctx;
	struct memory_info                    info;
	cap_id_t                              memory_cap;
	vmm_id_t                              buffer_id = VMM_ID_INVALID;
	uintptr_t                             buffer;
	uint8_t                               byte = 0xffu;

	kernel_capability_test_begin(&ctx, "kernel-cap/memory-zero");
	kernel_capability_test_poison_next_pmm_page(0xa7u);
	memory_cap = create_memory(CAP_CALL | CAP_READ, 3u);
	cr_assert_neq(memory_cap, CAP_ID_INVALID);
	cr_assert_eq(memory_info(memory_cap, &info).status, SYSCALL_STATUS_OK);
	cr_assert_eq(info.page_count, 3u);
	cr_assert_eq(info.memory_type, MEMORY_TYPE_NORMAL);
	buffer = kernel_capability_test_alloc_user_buffer(ctx.process, 1u, &buffer_id);
	cr_assert_eq(memory_read_to(memory_cap, 0u, buffer, 1u).status, SYSCALL_STATUS_OK);
	cr_assert_eq(address_space_copy_from(process_address_space(ctx.process), buffer, &byte, 1u), ADDRESS_TRANSFER_OK);
	cr_assert_eq(byte, 0u, "fresh memory object exposed recycled PMM contents");
	cr_assert(drop_capability(memory_cap));
	cr_assert(vm_space_unmap(process_address_space(ctx.process), buffer_id));
	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_memory, zero_grants_removes_routing_and_releases_unmapped_backing) {
	struct kernel_capability_test_context ctx;
	cap_id_t                              memory_cap;
	cap_object_id_t                       object_id;
	size_t                                free_before;
	uint8_t                               value = 0x5au;

	kernel_capability_test_begin(&ctx, "kernel-cap/memory-lifetime");
	free_before = pmm_free_size();
	memory_cap  = create_memory(CAP_CALL | CAP_WRITE, 1u);
	object_id   = capability_object_id(memory_cap);
	cr_assert_neq(object_id, CAP_OBJECT_ID_INVALID);
	struct cap_object* object = cap_object_acquire(object_id);
	cr_assert_not_null(object);
	struct memory_object* memory = (struct memory_object*)(uintptr_t)object->object_id;
	cr_assert(memory_object_write(memory, 0u, &value, 1u));
	cap_object_release(object);
	cr_assert_lt(pmm_free_size(), free_before);
	cr_assert(drop_capability(memory_cap));
	cr_assert_null(cap_object_acquire(object_id));
	cr_assert_eq(pmm_free_size(), free_before);
	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_memory, delegated_grants_keep_memory_routing_alive) {
	struct kernel_capability_test_context ctx;
	cap_id_t                              root_cap;
	cap_id_t                              delegated_cap;
	cap_object_id_t                       object_id;

	kernel_capability_test_begin(&ctx, "kernel-cap/memory-delegation");
	root_cap                = create_memory(CAP_CALL | CAP_READ | CAP_DELEGATE, 1u);
	object_id               = capability_object_id(root_cap);
	struct capability* root = cap_acquire(root_cap);
	cr_assert_not_null(root);
	delegated_cap = cap_delegate_create(root, process_pid(ctx.process), CAP_CALL | CAP_READ, false);
	cap_release(root);
	cr_assert_neq(delegated_cap, CAP_ID_INVALID);
	cr_assert(drop_capability(root_cap));
	struct cap_object* object = cap_object_acquire(object_id);
	cr_assert_not_null(object, "dropping creator grant destroyed a delegated memory object");
	cap_object_release(object);
	cr_assert(drop_capability(delegated_cap));
	cr_assert_null(cap_object_acquire(object_id));
	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_memory, mapping_and_memory_control_caps_do_not_own_the_mapping) {
	struct kernel_capability_test_context ctx;
	cap_id_t                              memory_cap;
	struct address_space_map_result       mapped;
	struct vmm_info                       live;
	cap_object_id_t                       mapping_object_id;
	uint8_t                               written = 0x6du;
	uint8_t                               read    = 0u;

	kernel_capability_test_begin(&ctx, "kernel-cap/mapping-independence");
	memory_cap                            = create_memory(CAP_MAP | CAP_READ | CAP_WRITE, 2u);
	const struct memory_map_params params = {
		.memory_page_offset = 0u, .page_count = 2u, .align_pages = 1u, .prot = VMM_PROT_READ | VMM_PROT_WRITE};
	cr_assert_eq(map_memory(memory_cap, process_pid(ctx.process), ctx.process, &params, &mapped).status,
	             SYSCALL_STATUS_OK);
	cr_assert_neq(mapped.mapping_cap, CAP_ID_INVALID);
	mapping_object_id = capability_object_id(mapped.mapping_cap);
	cr_assert(drop_capability(mapped.mapping_cap));
	cr_assert_null(cap_object_acquire(mapping_object_id), "zero-grant mapping control metadata remained published");
	cr_assert(vm_space_query(process_address_space(ctx.process), (uintptr_t)mapped.mapping.base, &live));
	cr_assert_eq(address_space_copy_to(
					 process_address_space(ctx.process), (uintptr_t)mapped.mapping.base, &written, sizeof(written)),
	             ADDRESS_TRANSFER_OK);
	cr_assert(drop_capability(memory_cap));
	cr_assert(vm_space_query(process_address_space(ctx.process), (uintptr_t)mapped.mapping.base, &live));
	cr_assert_eq(address_space_copy_from(
					 process_address_space(ctx.process), (uintptr_t)mapped.mapping.base, &read, sizeof(read)),
	             ADDRESS_TRANSFER_OK);
	cr_assert_eq(read, written);
	cr_assert(vm_space_unmap(process_address_space(ctx.process), live.id));
	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_memory, mapping_rights_bound_creation_and_later_protection) {
	struct kernel_capability_test_context ctx;
	cap_id_t                              no_map;
	cap_id_t                              read_only;
	struct address_space_map_result       mapped;

	kernel_capability_test_begin(&ctx, "kernel-cap/memory-rights");
	const struct memory_map_params read_params = {.page_count = 1u, .align_pages = 1u, .prot = VMM_PROT_READ};
	no_map                                     = create_memory(CAP_READ, 1u);
	cr_assert_eq(map_memory(no_map, process_pid(ctx.process), ctx.process, &read_params, &mapped).status,
	             SYSCALL_STATUS_DENIED);
	cr_assert(drop_capability(no_map));
	read_only = create_memory(CAP_MAP | CAP_READ, 1u);
	cr_assert_eq(map_memory(read_only, process_pid(ctx.process), ctx.process, &read_params, &mapped).status,
	             SYSCALL_STATUS_OK);
	cr_assert_eq(protect_mapping(mapped.mapping_cap, VMM_PROT_WRITE).status, SYSCALL_STATUS_DENIED);
	cr_assert_eq(protect_mapping(mapped.mapping_cap, VMM_PROT_EXEC).status, SYSCALL_STATUS_DENIED);
	cr_assert(drop_capability(read_only));
	cr_assert_eq(protect_mapping(mapped.mapping_cap, VMM_PROT_NONE).status, SYSCALL_STATUS_OK);
	cr_assert_eq(protect_mapping(mapped.mapping_cap, VMM_PROT_READ).status, SYSCALL_STATUS_OK);
	const struct mapping_unmap_request unmap = {.header = {.op = MAPPING_OP_UNMAP}};
	cr_assert_eq(kernel_capability_test_call(mapped.mapping_cap, &unmap, sizeof(unmap), NULL, 0u).status,
	             SYSCALL_STATUS_OK);
	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_memory, memory_read_write_rights_are_independent_of_info) {
	struct kernel_capability_test_context ctx;
	cap_id_t                              read_only;
	cap_id_t                              write_only;
	struct memory_info                    info;
	vmm_id_t                              buffer_id = VMM_ID_INVALID;
	uintptr_t                             buffer;

	kernel_capability_test_begin(&ctx, "kernel-cap/memory-io-rights");
	buffer     = kernel_capability_test_alloc_user_buffer(ctx.process, 1u, &buffer_id);
	read_only  = create_memory(CAP_CALL | CAP_READ, 1u);
	write_only = create_memory(CAP_CALL | CAP_WRITE, 1u);
	cr_assert_eq(memory_info(write_only, &info).status, SYSCALL_STATUS_OK);
	cr_assert_eq(memory_write_from(read_only, 0u, buffer, 1u).status, SYSCALL_STATUS_DENIED);
	cr_assert_eq(memory_read_to(write_only, 0u, buffer, 1u).status, SYSCALL_STATUS_DENIED);
	cr_assert(drop_capability(read_only));
	cr_assert(drop_capability(write_only));
	cr_assert(vm_space_unmap(process_address_space(ctx.process), buffer_id));
	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_memory, writes_synchronize_materialized_backing_without_protection_state) {
	struct kernel_capability_test_context ctx;
	cap_id_t                              memory_cap;
	vmm_id_t                              buffer_id = VMM_ID_INVALID;
	uintptr_t                             buffer;
	uint8_t                               value = 0x42u;

	kernel_capability_test_begin(&ctx, "kernel-cap/memory-executable-sync");
	buffer = kernel_capability_test_alloc_user_buffer(ctx.process, 1u, &buffer_id);
	cr_assert_eq(address_space_copy_to(process_address_space(ctx.process), buffer, &value, 1u), ADDRESS_TRANSFER_OK);
	memory_cap = create_memory(CAP_CALL | CAP_WRITE, 1u);
	cr_assert_eq(memory_write_from(memory_cap, 0u, buffer, 1u).status, SYSCALL_STATUS_OK);
	cr_assert_eq(kernel_capability_test_executable_sync_count(), 1u);
	cr_assert(drop_capability(memory_cap));
	cr_assert(vm_space_unmap(process_address_space(ctx.process), buffer_id));
	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_memory, partial_write_failure_synchronizes_the_successfully_modified_page) {
	struct kernel_capability_test_context ctx;
	cap_id_t                              memory_cap;
	vmm_id_t                              buffer_id = VMM_ID_INVALID;
	uintptr_t                             buffer;
	uint8_t                               value = 0x5au;

	kernel_capability_test_begin(&ctx, "kernel-cap/memory-partial-write-sync");
	buffer = kernel_capability_test_alloc_user_buffer(ctx.process, 1u, &buffer_id);
	for (size_t offset = 0u; offset < VMM_PAGE_SIZE; offset += sizeof(value)) {
		cr_assert_eq(address_space_copy_to(process_address_space(ctx.process), buffer + offset, &value, sizeof(value)),
		             ADDRESS_TRANSFER_OK);
	}
	memory_cap = create_memory(CAP_CALL | CAP_WRITE, 2u);
	cr_assert_eq(memory_write_from(memory_cap, 0u, buffer, VMM_PAGE_SIZE + 1u).status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(kernel_capability_test_executable_sync_count(), VMM_PAGE_SIZE / 256u);
	cr_assert(drop_capability(memory_cap));
	cr_assert(vm_space_unmap(process_address_space(ctx.process), buffer_id));
	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_memory, mapping_secondary_capability_resolution_does_not_require_cap_call) {
	struct kernel_capability_test_context ctx;
	struct address_space_map_result       mapped;
	cap_id_t                              memory_cap;

	kernel_capability_test_begin(&ctx, "kernel-cap/memory-direct-map");
	memory_cap                            = create_memory(CAP_MAP | CAP_READ, 1u);
	const struct memory_map_params params = {.page_count = 1u, .prot = VMM_PROT_READ};
	cr_assert_eq(map_memory(memory_cap, process_pid(ctx.process), ctx.process, &params, &mapped).status,
	             SYSCALL_STATUS_OK);
	cr_assert(drop_capability(memory_cap));
	const struct mapping_unmap_request unmap = {.header = {.op = MAPPING_OP_UNMAP}};
	cr_assert_eq(kernel_capability_test_call(mapped.mapping_cap, &unmap, sizeof(unmap), NULL, 0u).status,
	             SYSCALL_STATUS_OK);
	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_memory, generic_map_supports_subranges_exact_auto_alignment_and_guards) {
	struct kernel_capability_test_context ctx;
	cap_id_t                              memory_cap;
	struct address_space_map_result       exact;
	struct address_space_map_result       automatic;
	struct address_space_map_result       rejected;

	kernel_capability_test_begin(&ctx, "kernel-cap/memory-map-params");
	memory_cap                                  = create_memory(CAP_MAP | CAP_READ, 8u);
	const struct memory_map_params exact_params = {
		.memory_page_offset = 2u,
		.page_count         = 2u,
		.address            = 0x400000u,
		.align_pages        = 1u,
		.guard_pages        = 1u,
		.prot               = VMM_PROT_READ,
	};
	cr_assert_eq(map_memory(memory_cap, process_pid(ctx.process), ctx.process, &exact_params, &exact).status,
	             SYSCALL_STATUS_OK);
	cr_assert_eq((uintptr_t)exact.mapping.base, exact_params.address);
	cr_assert_eq(exact.mapping.page_count, 2u);
	cr_assert_eq(exact.mapping.memory_page_offset, 2u);
	cr_assert_eq(exact.mapping.guard_pages, 1u);
	cr_assert_eq(exact.mapping.prot, VMM_PROT_READ);
	cr_assert_eq(exact.mapping.memory_type, MEMORY_TYPE_NORMAL);
	struct address_space_map_result overlap;
	cr_assert_eq(map_memory(memory_cap, process_pid(ctx.process), ctx.process, &exact_params, &overlap).status,
	             SYSCALL_STATUS_BAD_ARGUMENT);
	const struct memory_map_params auto_params = {
		.memory_page_offset = 4u, .page_count = 1u, .align_pages = 8u, .guard_pages = 2u, .prot = VMM_PROT_READ};
	cr_assert_eq(map_memory(memory_cap, process_pid(ctx.process), ctx.process, &auto_params, &automatic).status,
	             SYSCALL_STATUS_OK);
	cr_assert_eq((uintptr_t)automatic.mapping.base % (8u * VMM_PAGE_SIZE), 0u);
	struct memory_map_params invalid = {.memory_page_offset = SIZE_MAX, .page_count = 2u, .prot = VMM_PROT_READ};
	cr_assert_eq(map_memory(memory_cap, process_pid(ctx.process), ctx.process, &invalid, &rejected).status,
	             SYSCALL_STATUS_BAD_ARGUMENT);
	const struct mapping_unmap_request unmap = {.header = {.op = MAPPING_OP_UNMAP}};
	cr_assert_eq(kernel_capability_test_call(exact.mapping_cap, &unmap, sizeof(unmap), NULL, 0u).status,
	             SYSCALL_STATUS_OK);
	cr_assert_eq(kernel_capability_test_call(automatic.mapping_cap, &unmap, sizeof(unmap), NULL, 0u).status,
	             SYSCALL_STATUS_OK);
	cr_assert(drop_capability(memory_cap));
	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_memory, target_death_invalidates_mapping_control_without_manual_unmap) {
	struct kernel_capability_test_context ctx;
	struct process*                       target;
	struct uthread*                       target_main;
	cap_id_t                              memory_cap;
	struct address_space_map_result       mapped;

	kernel_capability_test_begin(&ctx, "kernel-cap/mapping-target-death");
	target                                = syscall_test_spawn_process("kernel-cap/mapping-target");
	target_main                           = process_main_thread(target);
	memory_cap                            = create_memory(CAP_MAP | CAP_READ, 1u);
	const struct memory_map_params params = {.page_count = 1u, .prot = VMM_PROT_READ};
	cr_assert_eq(map_memory(memory_cap, process_pid(ctx.process), target, &params, &mapped).status, SYSCALL_STATUS_OK);
	thread_mark_zombie(&target_main->thread);
	cr_assert(process_destroy(target));
	cr_assert_null(cap_acquire(mapped.mapping_cap));
	cr_assert(drop_capability(memory_cap));
	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_memory, mapping_holder_death_does_not_unmap_another_live_space) {
	struct kernel_capability_test_context ctx;
	struct process*                       holder;
	struct process*                       target;
	struct uthread*                       holder_main;
	struct uthread*                       target_main;
	cap_id_t                              memory_cap;
	cap_id_t                              holder_memory;
	struct address_space_map_result       mapped;
	struct vmm_info                       live;

	kernel_capability_test_begin(&ctx, "kernel-cap/mapping-holder-death");
	holder                  = syscall_test_spawn_process("kernel-cap/mapping-holder");
	target                  = syscall_test_spawn_process("kernel-cap/mapping-target-live");
	holder_main             = process_main_thread(holder);
	target_main             = process_main_thread(target);
	memory_cap              = create_memory(CAP_MAP | CAP_READ | CAP_DELEGATE, 1u);
	struct capability* root = cap_acquire(memory_cap);
	holder_memory           = cap_delegate_create(root, process_pid(holder), CAP_MAP | CAP_READ, false);
	cap_release(root);
	const struct memory_map_params params = {.page_count = 1u, .prot = VMM_PROT_READ};
	cr_assert_eq(map_memory(holder_memory, process_pid(holder), target, &params, &mapped).status, SYSCALL_STATUS_OK);
	thread_mark_zombie(&holder_main->thread);
	cr_assert(process_destroy(holder));
	cr_assert(vm_space_query(process_address_space(target), (uintptr_t)mapped.mapping.base, &live));
	cr_assert_null(cap_acquire(mapped.mapping_cap));
	cr_assert(drop_capability(memory_cap));
	thread_mark_zombie(&target_main->thread);
	cr_assert(process_destroy(target));
	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_memory, process_teardown_drops_memory_grants_through_generic_cleanup) {
	struct kernel_capability_test_context ctx;
	struct process*                       holder;
	struct uthread*                       holder_main;
	cap_id_t                              root_cap;
	cap_id_t                              holder_cap;
	cap_object_id_t                       object_id;

	kernel_capability_test_begin(&ctx, "kernel-cap/memory-process-cleanup");
	holder                  = syscall_test_spawn_process("kernel-cap/memory-holder");
	holder_main             = process_main_thread(holder);
	root_cap                = create_memory(CAP_READ | CAP_DELEGATE, 1u);
	object_id               = capability_object_id(root_cap);
	struct capability* root = cap_acquire(root_cap);
	holder_cap              = cap_delegate_create(root, process_pid(holder), CAP_READ, false);
	cap_release(root);
	cr_assert_neq(holder_cap, CAP_ID_INVALID);
	cr_assert(drop_capability(root_cap));
	thread_mark_zombie(&holder_main->thread);
	cr_assert(process_destroy(holder));
	cr_assert_null(cap_object_acquire(object_id));
	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_memory, one_memory_object_shares_backing_across_address_spaces) {
	struct kernel_capability_test_context ctx;
	struct process*                       other;
	struct uthread*                       other_main;
	cap_id_t                              memory_cap;
	struct address_space_map_result       first;
	struct address_space_map_result       second;
	struct vmm_info                       first_live;
	struct vmm_info                       second_live;
	uint8_t                               value = 0x91u;
	uint8_t                               observed;

	kernel_capability_test_begin(&ctx, "kernel-cap/memory-sharing");
	other                                 = syscall_test_spawn_process("kernel-cap/memory-sharing-peer");
	other_main                            = process_main_thread(other);
	memory_cap                            = create_memory(CAP_MAP | CAP_READ | CAP_WRITE, 1u);
	const struct memory_map_params params = {
		.page_count = 1u, .align_pages = 1u, .prot = VMM_PROT_READ | VMM_PROT_WRITE};
	cr_assert_eq(map_memory(memory_cap, process_pid(ctx.process), ctx.process, &params, &first).status,
	             SYSCALL_STATUS_OK);
	cr_assert_eq(map_memory(memory_cap, process_pid(ctx.process), other, &params, &second).status, SYSCALL_STATUS_OK);
	cr_assert(drop_capability(first.mapping_cap));
	cr_assert(drop_capability(second.mapping_cap));
	cr_assert(drop_capability(memory_cap));
	cr_assert_eq(
		address_space_copy_to(process_address_space(ctx.process), (uintptr_t)first.mapping.base, &value, sizeof(value)),
		ADDRESS_TRANSFER_OK);
	cr_assert_eq(address_space_copy_from(
					 process_address_space(other), (uintptr_t)second.mapping.base, &observed, sizeof(observed)),
	             ADDRESS_TRANSFER_OK);
	cr_assert_eq(observed, value);
	cr_assert(vm_space_query(process_address_space(ctx.process), (uintptr_t)first.mapping.base, &first_live));
	cr_assert(vm_space_query(process_address_space(other), (uintptr_t)second.mapping.base, &second_live));
	cr_assert(vm_space_unmap(process_address_space(ctx.process), first_live.id));
	cr_assert_eq(address_space_copy_from(
					 process_address_space(other), (uintptr_t)second.mapping.base, &observed, sizeof(observed)),
	             ADDRESS_TRANSFER_OK);
	cr_assert_eq(observed, value);
	thread_mark_zombie(&other_main->thread);
	cr_assert(process_destroy(other));
	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_memory, delegated_mapping_rights_reduce_the_protection_ceiling) {
	struct kernel_capability_test_context ctx;
	cap_id_t                              memory_cap;
	struct address_space_map_result       mapped;

	kernel_capability_test_begin(&ctx, "kernel-cap/mapping-protection-ceiling");
	memory_cap                            = create_memory(CAP_MAP | CAP_READ | CAP_WRITE, 1u);
	const struct memory_map_params params = {.page_count = 1u, .prot = VMM_PROT_READ | VMM_PROT_WRITE};
	cr_assert_eq(map_memory(memory_cap, process_pid(ctx.process), ctx.process, &params, &mapped).status,
	             SYSCALL_STATUS_OK);
	cr_assert(drop_capability(memory_cap));
	cr_assert_eq(protect_mapping(mapped.mapping_cap, VMM_PROT_NONE).status, SYSCALL_STATUS_OK);
	cr_assert_eq(protect_mapping(mapped.mapping_cap, VMM_PROT_READ | VMM_PROT_WRITE).status, SYSCALL_STATUS_OK);
	struct capability* mapping = cap_acquire(mapped.mapping_cap);
	cr_assert_not_null(mapping);
	cap_id_t reduced =
		cap_delegate_create(mapping, process_pid(ctx.process), CAP_CALL | CAP_MAP | CAP_READ | CAP_DESTROY, false);
	cap_release(mapping);
	cr_assert_neq(reduced, CAP_ID_INVALID);
	cr_assert_eq(protect_mapping(reduced, VMM_PROT_WRITE).status, SYSCALL_STATUS_DENIED);
	cr_assert_eq(protect_mapping(reduced, VMM_PROT_READ).status, SYSCALL_STATUS_OK);
	const struct mapping_unmap_request unmap = {.header = {.op = MAPPING_OP_UNMAP}};
	cr_assert_eq(kernel_capability_test_call(mapped.mapping_cap, &unmap, sizeof(unmap), NULL, 0u).status,
	             SYSCALL_STATUS_OK);
	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_memory, mapping_control_state_is_only_two_identity_fields) {
	cr_assert_eq(kernel_mapping_state_size(), sizeof(process_id_t) + sizeof(vmm_id_t));
}

Test(kernel_capability_memory, fixed_device_memory_is_exclusive_and_controls_mapping_type) {
	struct kernel_capability_test_context ctx;
	struct address_space_map_result       mapped;
	struct memory_info                    info;
	cap_id_t                              first;
	cap_id_t                              second;

	kernel_capability_test_begin(&ctx, "kernel-cap/memory-fixed-device");
	const struct memory_create_params create_params = {
		.page_count  = 2u,
		.memory_type = MEMORY_TYPE_DEVICE,
		.constraints = {.physical_address = 0x500000u, .flags = MEMORY_CONSTRAINT_FIXED},
	};
	first = create_memory_params(CAP_CALL | CAP_READ | CAP_WRITE | CAP_EXEC | CAP_MAP, &create_params);
	cr_assert_neq(first, CAP_ID_INVALID);
	cr_assert_eq(memory_info(first, &info).status, SYSCALL_STATUS_OK);
	cr_assert_eq(info.page_count, 2u);
	cr_assert_eq(info.memory_type, MEMORY_TYPE_DEVICE);
	second = create_memory_params(CAP_CALL | CAP_MAP, &create_params);
	cr_assert_eq(second, CAP_ID_INVALID);

	const struct memory_map_params map_params = {.page_count = 2u, .prot = VMM_PROT_READ};
	cr_assert_eq(map_memory(first, process_pid(ctx.process), ctx.process, &map_params, &mapped).status,
	             SYSCALL_STATUS_OK);
	cr_assert_eq(mapped.mapping.memory_type, MEMORY_TYPE_DEVICE);
	uint8_t ignored;
	cr_assert_eq(
		address_space_copy_from(process_address_space(ctx.process), (uintptr_t)mapped.mapping.base, &ignored, 1u),
		ADDRESS_TRANSFER_ACCESS_DENIED);
	cr_assert_eq(protect_mapping(mapped.mapping_cap, VMM_PROT_EXEC).status, SYSCALL_STATUS_BAD_ARGUMENT);
	const struct mapping_unmap_request unmap = {.header = {.op = MAPPING_OP_UNMAP}};
	cr_assert_eq(kernel_capability_test_call(mapped.mapping_cap, &unmap, sizeof(unmap), NULL, 0u).status,
	             SYSCALL_STATUS_OK);
	cr_assert(drop_capability(first));
	second = create_memory_params(CAP_CALL | CAP_MAP, &create_params);
	cr_assert_neq(second, CAP_ID_INVALID);
	cr_assert(drop_capability(second));
	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_memory, create_syscall_rejects_invalid_physical_constraints) {
	struct kernel_capability_test_context ctx;

	kernel_capability_test_begin(&ctx, "kernel-cap/memory-create-invalid-constraints");
	const struct memory_create_params params = {
		.page_count = 1u,
		.constraints =
			{
						  .physical_min = VMM_PAGE_SIZE,
						  .align_pages  = 2u,
						  },
	};
	syscall_result_t result = syscall_memory_create((uintptr_t)&params, sizeof(params), 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	kernel_capability_test_end(&ctx);
}
