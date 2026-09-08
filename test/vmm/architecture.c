#include <base/vmm.h>

#include "test_support.h"

static _Alignas(VMM_PAGE_SIZE) uint8_t arena[KiB(192)];

static bool map_memory(struct address_space* space, struct memory* memory, size_t offset, size_t pages,
                       uintptr_t requested, size_t align, size_t guards, vmm_prot_t prot, vmm_id_t* id, void** base) {
	return vm_space_map(space,
	                    &(const struct vm_map_request){
							.memory             = memory,
							.memory_page_offset = offset,
							.page_count         = pages,
							.requested_base     = requested,
							.align_pages        = align,
							.guard_pages        = guards,
							.prot               = prot,
						},
	                    id,
	                    base);
}

Test(vmm, mapping_vector_placement_alignment_guards_and_overlap) {
	struct memory*  memory;
	vmm_id_t        fixed_id, first_id, aligned_id;
	void *          first, *aligned;
	struct vmm_info info[3];
	init_test_vmm(arena, sizeof(arena));
	cr_assert(memory_create_anonymous(16u * VMM_PAGE_SIZE, &memory));
	uintptr_t fixed = MM_KERNEL_VMM_BASE + 8u * VMM_PAGE_SIZE;
	cr_assert(
		map_memory(vm_space_kernel(), memory, 0u, 2u, fixed, 1u, 2u, VMM_PROT_READ | VMM_PROT_WRITE, &fixed_id, NULL));
	cr_assert_not(
		map_memory(vm_space_kernel(), memory, 0u, 1u, fixed - VMM_PAGE_SIZE, 1u, 0u, VMM_PROT_READ, &first_id, NULL),
		"exact mapping overlapped a guard");
	cr_assert(map_memory(vm_space_kernel(), memory, 2u, 1u, 0u, 1u, 1u, VMM_PROT_READ, &first_id, &first));
	cr_assert(map_memory(vm_space_kernel(), memory, 3u, 1u, 0u, 8u, 0u, VMM_PROT_READ, &aligned_id, &aligned));
	cr_assert_eq((uintptr_t)aligned & (8u * VMM_PAGE_SIZE - 1u), 0u);
	cr_assert_eq(vm_space_mapping_count(vm_space_kernel()), 3u);
	for (size_t i = 0u; i < 3u; i++) cr_assert(vm_space_query_at(vm_space_kernel(), i, &info[i]));
	cr_assert_lt((uintptr_t)info[0].base, (uintptr_t)info[1].base);
	cr_assert_lt((uintptr_t)info[1].base, (uintptr_t)info[2].base);
	cr_assert_not(vm_space_query(vm_space_kernel(), fixed - VMM_PAGE_SIZE, &info[0]), "guard became usable");
	cr_assert(vm_space_unmap(vm_space_kernel(), fixed_id));
	cr_assert(vm_space_unmap(vm_space_kernel(), first_id));
	cr_assert(vm_space_unmap(vm_space_kernel(), aligned_id));
	memory_release(memory);
}

Test(vmm, mapping_rejects_a_view_with_an_unaligned_backing_offset) {
	struct memory *root, *slice;
	vmm_id_t       id;
	void*          base;
	init_test_vmm(arena, sizeof(arena));
	cr_assert(memory_create_anonymous(VMM_PAGE_SIZE + 1u, &root));
	cr_assert(memory_slice(root, 1u, VMM_PAGE_SIZE, &slice));
	cr_assert_not(map_memory(vm_space_kernel(), slice, 0u, 1u, 0u, 1u, 0u, VMM_PROT_READ, &id, &base));
	memory_release(slice);
	memory_release(root);
}

Test(vmm, huge_sparse_mapping_uses_constant_initial_metadata) {
	struct memory* memory;
	vmm_id_t       id;
	void*          base;
	init_test_vmm(arena, sizeof(arena));
	size_t before = pmm_free_size();
	cr_assert(memory_create_anonymous(200000u * VMM_PAGE_SIZE, &memory));
	cr_assert(map_memory(vm_space_kernel(), memory, 0u, 200000u, 0u, 1u, 0u, VMM_PROT_READ, &id, &base));
	cr_assert_eq(before - pmm_free_size(),
	             3u * VMM_PAGE_SIZE,
	             "object, backing, and mapping vector should use three control pages");
	cr_assert_not(hal_paging_query(vm_space_hal(vm_space_kernel()), (uintptr_t)base, NULL));
	cr_assert(vm_space_unmap(vm_space_kernel(), id));
	memory_release(memory);
	cr_assert_eq(pmm_free_size(), before);
}

Test(vmm, owned_object_implicit_zero_and_sparse_write) {
	struct memory* memory;
	uint8_t        readback[32];
	uint8_t        value[3] = {1u, 2u, 3u};
	init_test_vmm(arena, sizeof(arena));
	size_t before = pmm_free_size();
	cr_assert(memory_create_anonymous(4096u * VMM_PAGE_SIZE, &memory));
	size_t after_control = pmm_free_size();
	cr_assert(memory_read(memory, 17u * VMM_PAGE_SIZE + 7u, readback, sizeof(readback)));
	for (size_t i = 0u; i < sizeof(readback); i++) cr_assert_eq(readback[i], 0u);
	cr_assert_eq(pmm_free_size(), after_control, "zero read materialized backing");
	cr_assert(memory_write(memory, 17u * VMM_PAGE_SIZE + VMM_PAGE_SIZE - 1u, value, sizeof(value)));
	cr_assert(memory_read(memory, 17u * VMM_PAGE_SIZE + VMM_PAGE_SIZE - 1u, readback, sizeof(value)));
	cr_assert_arr_eq(readback, value, sizeof(value));
	cr_assert_lt(pmm_free_size(), after_control);
	memory_release(memory);
	cr_assert_eq(pmm_free_size(), before);
}

Test(vmm, shared_object_faults_reuse_backing_and_survive_unmap) {
	struct address_space          a = {0}, b = {0};
	struct memory*                memory;
	vmm_id_t                      a_id, b_id, remap_id;
	void *                        a_base, *b_base, *remap_base;
	struct hal_paging_translation a_translation, b_translation, remap_translation;
	uint8_t                       written = 0x5au, readback = 0u;
	init_test_vmm(arena, sizeof(arena));
	cr_assert(vm_space_create_user(&a));
	cr_assert(vm_space_create_user(&b));
	cr_assert(memory_create_anonymous(2u * VMM_PAGE_SIZE, &memory));
	cr_assert(map_memory(&a, memory, 0u, 2u, 0u, 1u, 0u, VMM_PROT_READ | VMM_PROT_WRITE, &a_id, &a_base));
	cr_assert(map_memory(&b, memory, 0u, 2u, 0u, 1u, 0u, VMM_PROT_READ | VMM_PROT_WRITE, &b_id, &b_base));
	cr_assert_not(hal_paging_query(vm_space_hal(&a), (uintptr_t)a_base, NULL));
	cr_assert(vm_space_resolve_page_fault(&a, (uintptr_t)a_base, VMM_FAULT_ACCESS_WRITE));
	cr_assert(vm_space_resolve_page_fault(&b, (uintptr_t)b_base, VMM_FAULT_ACCESS_READ));
	cr_assert(hal_paging_query(vm_space_hal(&a), (uintptr_t)a_base, &a_translation));
	cr_assert(hal_paging_query(vm_space_hal(&b), (uintptr_t)b_base, &b_translation));
	cr_assert_eq(a_translation.physical_address, b_translation.physical_address);
	cr_assert(memory_write(memory, 0u, &written, 1u));
	cr_assert(memory_read(memory, 0u, &readback, 1u));
	cr_assert_eq(readback, written);
	cr_assert(vm_space_unmap(&a, a_id));
	cr_assert(hal_paging_query(vm_space_hal(&b), (uintptr_t)b_base, &b_translation));
	cr_assert(map_memory(&a, memory, 0u, 2u, 0u, 1u, 0u, VMM_PROT_READ, &remap_id, &remap_base));
	cr_assert(vm_space_resolve_page_fault(&a, (uintptr_t)remap_base, VMM_FAULT_ACCESS_READ));
	cr_assert(hal_paging_query(vm_space_hal(&a), (uintptr_t)remap_base, &remap_translation));
	cr_assert_eq(remap_translation.physical_address, b_translation.physical_address);
	cr_assert(vm_space_unmap(&a, remap_id));
	vm_space_destroy(&a);
	cr_assert(hal_paging_query(vm_space_hal(&b), (uintptr_t)b_base, NULL));
	cr_assert(vm_space_unmap(&b, b_id));
	vm_space_destroy(&b);
	memory_release(memory);
}

Test(vmm, protect_updates_present_and_future_pages) {
	struct memory*                memory;
	vmm_id_t                      id;
	void*                         base;
	struct hal_paging_translation translation;
	init_test_vmm(arena, sizeof(arena));
	cr_assert(memory_create_anonymous(2u * VMM_PAGE_SIZE, &memory));
	cr_assert(map_memory(vm_space_kernel(), memory, 0u, 2u, 0u, 1u, 0u, VMM_PROT_READ | VMM_PROT_WRITE, &id, &base));
	cr_assert(vm_space_resolve_page_fault(vm_space_kernel(), (uintptr_t)base, VMM_FAULT_ACCESS_WRITE));
	cr_assert(vm_space_protect(vm_space_kernel(), id, VMM_PROT_READ | VMM_PROT_EXEC));
	cr_assert(hal_paging_query(vm_space_hal(vm_space_kernel()), (uintptr_t)base, &translation));
	cr_assert_eq(translation.flags, HAL_PAGE_READ | HAL_PAGE_EXEC);
	cr_assert(vm_space_resolve_page_fault(vm_space_kernel(), (uintptr_t)base + VMM_PAGE_SIZE, VMM_FAULT_ACCESS_READ));
	cr_assert(hal_paging_query(vm_space_hal(vm_space_kernel()), (uintptr_t)base + VMM_PAGE_SIZE, &translation));
	cr_assert_eq(translation.flags, HAL_PAGE_READ | HAL_PAGE_EXEC);
	cr_assert(vm_space_unmap(vm_space_kernel(), id));
	cr_assert(memory_read(memory, 0u, &translation, sizeof(uint64_t)), "unmap discarded memory contents");
	memory_release(memory);
}

Test(vmm, failed_pte_install_keeps_resolved_object_page) {
	struct memory* memory;
	vmm_id_t       id;
	void*          base;
	init_test_vmm(arena, sizeof(arena));
	cr_assert(memory_create_anonymous(VMM_PAGE_SIZE, &memory));
	cr_assert(map_memory(vm_space_kernel(), memory, 0u, 1u, 0u, 1u, 0u, VMM_PROT_READ, &id, &base));
	mock_paging_fail_after(0u);
	cr_assert_not(vm_space_resolve_page_fault(vm_space_kernel(), (uintptr_t)base, VMM_FAULT_ACCESS_READ));
	struct memory_span span;
	cr_assert(memory_query(memory, 0u, VMM_PAGE_SIZE, &span) && span.kind == MEMORY_SPAN_PRESENT,
	          "failed PTE install rolled back valid memory data");
	cr_assert(vm_space_unmap(vm_space_kernel(), id));
	memory_release(memory);
}

Test(vmm, external_memory_has_direct_backing_and_never_owns_frames) {
	struct memory*     memory;
	struct memory_span span;
	init_test_vmm(arena, sizeof(arena));
	size_t before = pmm_free_size();
	cr_assert(memory_create_physical(
		&(const struct memory_physical_request){
			.physical_address = 0x400000u,
			.size             = 3u * VMM_PAGE_SIZE,
			.memory_type      = MEMORY_TYPE_NORMAL,
		},
		&memory));
	cr_assert_not(memory_can_transfer(memory));
	cr_assert(memory_query(memory, 2u * VMM_PAGE_SIZE, VMM_PAGE_SIZE, &span));
	cr_assert_eq(span.physical_address, 0x400000u + 2u * VMM_PAGE_SIZE);
	cr_assert_eq(before - pmm_free_size(), 2u * VMM_PAGE_SIZE);
	memory_release(memory);
	cr_assert_eq(pmm_free_size(), before);
}

Test(vmm, external_helper_backing_is_usable_by_address_transfer) {
	struct address_space                   space = {0};
	struct memory*                         memory;
	vmm_id_t                               id;
	void*                                  base;
	uint8_t                                copied[32];
	static _Alignas(VMM_PAGE_SIZE) uint8_t external[VMM_PAGE_SIZE];

	init_test_vmm(arena, sizeof(arena));
	for (size_t i = 0u; i < sizeof(external); i++) external[i] = (uint8_t)i;
	cr_assert(memory_create_physical(
		&(const struct memory_physical_request){
			.physical_address        = (uintptr_t)external,
			.size                    = sizeof(external),
			.memory_type             = MEMORY_TYPE_NORMAL,
			.external_cpu_accessible = true,
		},
		&memory));
	cr_assert(memory_can_transfer(memory));
	cr_assert(vm_space_create_user(&space));
	cr_assert(map_memory(&space, memory, 0u, 1u, 0u, 1u, 0u, VMM_PROT_READ, &id, &base));
	cr_assert_eq(address_space_copy_from(&space, (uintptr_t)base + 17u, copied, sizeof(copied)), ADDRESS_TRANSFER_OK);
	cr_assert_arr_eq(copied, external + 17u, sizeof(copied));
	cr_assert(vm_space_unmap(&space, id));
	vm_space_destroy(&space);
	memory_release(memory);
}

Test(vmm, constrained_memory_reserves_contiguous_backing) {
	struct memory*                         memory;
	struct memory_span                     first;
	struct memory_span                     last;
	static _Alignas(VMM_PAGE_SIZE) uint8_t constrained_arena[KiB(192)];

	init_test_vmm(constrained_arena, sizeof(constrained_arena));
	cr_assert(memory_create_anonymous(3u * VMM_PAGE_SIZE, &memory));
	cr_assert(memory_materialize(memory,
	                             &(const struct memory_materialize_request){
									 .size               = 3u * VMM_PAGE_SIZE,
									 .alignment          = 2u * VMM_PAGE_SIZE,
									 .minimum_address    = (uintptr_t)constrained_arena,
									 .maximum_address    = (uintptr_t)constrained_arena + sizeof(constrained_arena),
									 .require_contiguous = true,
								 }));
	cr_assert_eq(memory_type(memory), MEMORY_TYPE_NORMAL);
	cr_assert(memory_query(memory, 0u, VMM_PAGE_SIZE, &first));
	cr_assert(memory_query(memory, 2u * VMM_PAGE_SIZE, VMM_PAGE_SIZE, &last));
	cr_assert_eq(last.physical_address, first.physical_address + 2u * VMM_PAGE_SIZE);
	cr_assert_eq(first.physical_address & (2u * VMM_PAGE_SIZE - 1u), 0u);
	memory_release(memory);
}

Test(vmm, bounded_memory_materializes_each_page_inside_the_window) {
	struct memory*                         memory;
	struct memory_span                     span;
	static _Alignas(VMM_PAGE_SIZE) uint8_t bounded_arena[KiB(192)];

	init_test_vmm(bounded_arena, sizeof(bounded_arena));
	cr_assert(memory_create_anonymous(3u * VMM_PAGE_SIZE, &memory));
	for (size_t page = 0u; page < 3u; page++) {
		cr_assert(memory_materialize(memory,
		                             &(const struct memory_materialize_request){
										 .offset          = page * VMM_PAGE_SIZE,
										 .size            = VMM_PAGE_SIZE,
										 .alignment       = VMM_PAGE_SIZE,
										 .minimum_address = (uintptr_t)(bounded_arena + KiB(64)),
										 .maximum_address = (uintptr_t)(bounded_arena + sizeof(bounded_arena)),
									 }));
		cr_assert(memory_query(memory, page * VMM_PAGE_SIZE, VMM_PAGE_SIZE, &span));
		cr_assert_geq(span.physical_address, (uintptr_t)(bounded_arena + KiB(64)));
		cr_assert_lt(span.physical_address, (uintptr_t)(bounded_arena + sizeof(bounded_arena)));
	}
	memory_release(memory);
}

Test(vmm, fixed_external_memories_are_exclusive_and_support_physical_zero) {
	struct memory*                         first;
	struct memory*                         second;
	struct memory*                         zero;
	struct memory_span                     span;
	static _Alignas(VMM_PAGE_SIZE) uint8_t fixed_arena[KiB(192)];

	init_test_vmm(fixed_arena, sizeof(fixed_arena));
	const struct memory_physical_request fixed = {
		.physical_address = 0x400000u,
		.size             = 2u * VMM_PAGE_SIZE,
		.memory_type      = MEMORY_TYPE_DEVICE,
	};
	cr_assert(memory_create_physical(&fixed, &first));
	cr_assert_eq(memory_type(first), MEMORY_TYPE_DEVICE);
	cr_assert_not(memory_create_physical(&fixed, &second));
	memory_release(first);
	cr_assert(memory_create_physical(&fixed, &second));
	memory_release(second);

	struct memory_physical_request at_zero = fixed;
	at_zero.size                           = VMM_PAGE_SIZE;
	at_zero.physical_address               = 0u;
	cr_assert(memory_create_physical(&at_zero, &zero));
	cr_assert(memory_query(zero, 0u, VMM_PAGE_SIZE, &span));
	cr_assert_eq(span.physical_address, 0u);
	memory_release(zero);
}
