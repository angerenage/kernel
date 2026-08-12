#include "../../kernel/src/capability/memory.h"

#include "test_support.h"

static cap_id_t allocate_test_page(void) {
	return kernel_allocate_memory(CAP_CALL | CAP_READ | CAP_WRITE | CAP_MAP | CAP_DESTROY,
	                              1u,
	                              VMM_PROT_READ | VMM_PROT_WRITE | VMM_PROT_USER,
	                              VMM_KIND_GENERIC);
}

static syscall_result_t free_allocation(cap_id_t cap) {
	const struct allocation_free_request request = {.header = {.op = ALLOCATION_OP_FREE}};
	return kernel_capability_test_call(cap, &request, sizeof(request), NULL, 0u);
}

Test(kernel_capability_memory, newly_allocated_user_memory_does_not_expose_recycled_page_contents) {
	struct kernel_capability_test_context ctx;
	struct allocation_copy_from_request   request;
	struct allocation_copy_response       response;
	syscall_result_t                      result;
	cap_id_t                              allocation_cap;
	vmm_id_t                              dst_id = VMM_ID_INVALID;
	uintptr_t                             dst;
	uint8_t                               byte   = 0xffu;
	const uint8_t                         poison = 0xa7u;

	kernel_capability_test_begin(&ctx, "kernel-cap/memory-zero");
	kernel_capability_test_poison_next_pmm_page(poison);

	allocation_cap = allocate_test_page();
	cr_assert_neq(allocation_cap, CAP_ID_INVALID);
	dst = kernel_capability_test_alloc_user_buffer(ctx.process, 1u, &dst_id);

	request = (struct allocation_copy_from_request){
		.header      = {.op = ALLOCATION_OP_COPY_FROM},
		.src_offset  = 0u,
		.dst_address = dst,
		.size        = 1u,
	};
	result = kernel_capability_test_call(allocation_cap, &request, sizeof(request), &response, sizeof(response));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(response.bytes_copied, 1u);
	cr_assert_eq(address_space_copy_from(process_address_space(ctx.process), dst, &byte, sizeof(byte)),
	             ADDRESS_TRANSFER_OK);
	cr_assert_eq(byte, 0u, "fresh userspace allocation exposed recycled physical memory (0x%02x)", byte);

	cr_assert_eq(free_allocation(allocation_cap).status, SYSCALL_STATUS_OK);
	if (dst_id != VMM_ID_INVALID) cr_assert(vmm_free(process_address_space(ctx.process), dst_id));
	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_memory, allocation_free_invalidates_the_released_capability_and_routing_object) {
	struct kernel_capability_test_context ctx;
	cap_id_t                              allocation_cap;
	struct capability*                    cap;
	cap_object_id_t                       object_id;
	size_t                                caps_before;
	size_t                                objects_before;
	syscall_result_t                      result;

	kernel_capability_test_begin(&ctx, "kernel-cap/allocation-free");
	caps_before    = capability_count();
	objects_before = capability_object_count();

	allocation_cap = allocate_test_page();
	cr_assert_neq(allocation_cap, CAP_ID_INVALID);
	cap = cap_acquire(allocation_cap);
	cr_assert_not_null(cap);
	object_id = cap->cap_object_id;
	cap_release(cap);
	cr_assert_eq(capability_count(), caps_before + 1u);
	cr_assert_eq(capability_object_count(), objects_before + 1u);

	result = free_allocation(allocation_cap);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cap = cap_acquire(allocation_cap);
	cr_assert_null(cap, "allocation_free is documented to destroy the released capability");
	cr_assert_null(cap_object_acquire(object_id), "released allocation left a live routing object");
	cr_assert_eq(capability_count(), caps_before);
	cr_assert_eq(capability_object_count(), objects_before);

	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_memory, mapping_unmap_invalidates_the_mapping_capability_without_blocking_allocation_free) {
	struct kernel_capability_test_context ctx;
	const struct mapping_unmap_request    unmap_request = {.header = {.op = MAPPING_OP_UNMAP}};
	cap_id_t                              allocation_cap;
	cap_id_t                              mapping_cap = CAP_ID_INVALID;
	struct capability*                    mapping;
	cap_object_id_t                       mapping_object_id;
	syscall_result_t                      result;

	kernel_capability_test_begin(&ctx, "kernel-cap/mapping-unmap");
	allocation_cap = allocate_test_page();
	cr_assert_neq(allocation_cap, CAP_ID_INVALID);

	result = kernel_map_allocation(allocation_cap, process_pid(ctx.process), ctx.process, 0u, &mapping_cap);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_neq(mapping_cap, CAP_ID_INVALID);
	mapping = cap_acquire(mapping_cap);
	cr_assert_not_null(mapping);
	mapping_object_id = mapping->cap_object_id;
	cap_release(mapping);

	result = kernel_capability_test_call(mapping_cap, &unmap_request, sizeof(unmap_request), NULL, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_null(cap_acquire(mapping_cap), "unmapped mapping capability remained usable");
	cr_assert_null(cap_object_acquire(mapping_object_id), "unmapped mapping left a live routing object");
	cr_assert_eq(free_allocation(allocation_cap).status,
	             SYSCALL_STATUS_OK,
	             "removed mapping left a phantom allocation mapping reference");

	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_memory, target_process_teardown_releases_mapping_ownership) {
	struct kernel_capability_test_context ctx;
	struct process*                       target;
	struct uthread*                       target_main;
	cap_id_t                              allocation_cap;
	cap_id_t                              mapping_cap = CAP_ID_INVALID;
	syscall_result_t                      result;

	kernel_capability_test_begin(&ctx, "kernel-cap/mapping-target-lifetime");
	target = syscall_test_spawn_process("kernel-cap/mapping-target");
	cr_assert_not_null(target);
	target_main = process_main_thread(target);
	cr_assert_not_null(target_main);

	allocation_cap = allocate_test_page();
	cr_assert_neq(allocation_cap, CAP_ID_INVALID);
	result = kernel_map_allocation(allocation_cap, process_pid(ctx.process), target, 0u, &mapping_cap);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_neq(mapping_cap, CAP_ID_INVALID);

	thread_mark_zombie(&target_main->thread);
	cr_assert(process_destroy(target), "failed to destroy mapping target process");

	result = free_allocation(allocation_cap);
	cr_assert_eq(result.status,
	             SYSCALL_STATUS_OK,
	             "destroyed target address space left a phantom mapping reference on the allocation");

	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_memory, process_teardown_reclaims_unfreed_allocation_backing) {
	struct kernel_capability_test_context ctx;
	cap_id_t                              allocation_cap;
	size_t                                free_before_process;

	/* Record the PMM baseline before the process owns stacks/address-space resources. */
	syscall_test_init_process_environment();
	capability_init();
	free_before_process = pmm_free_page_count();

	ctx             = (struct kernel_capability_test_context){0};
	ctx.process     = syscall_test_spawn_process("kernel-cap/process-allocation-owner");
	ctx.main_thread = process_main_thread(ctx.process);
	cr_assert_not_null(ctx.main_thread);
	sched_set_current(cpu_current(), &ctx.main_thread->thread);
	ctx.main_thread->thread.address_space = NULL;

	allocation_cap = allocate_test_page();
	cr_assert_neq(allocation_cap, CAP_ID_INVALID);

	thread_mark_zombie(&ctx.main_thread->thread);
	sched_set_current(cpu_current(), NULL);
	cr_assert(process_destroy(ctx.process));
	ctx = (struct kernel_capability_test_context){0};

	cr_assert_eq(pmm_free_page_count(),
	             free_before_process,
	             "process teardown leaked physical memory owned by an unreleased allocation capability");
	syscall_test_reset_state();
}
