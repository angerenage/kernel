#include <base/vmm.h>

#include "test_support.h"

static _Alignas(VMM_PAGE_SIZE) uint8_t arena[KiB(192)];

static bool map_object(struct address_space* space, struct memory_object* memory, size_t offset, size_t pages,
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
	struct memory_object* memory;
	vmm_id_t              fixed_id, first_id, aligned_id;
	void *                first, *aligned;
	struct vmm_info       info[3];
	init_test_vmm(arena, sizeof(arena));
	cr_assert(memory_object_create_owned(16u, &memory));
	uintptr_t fixed = MM_KERNEL_VMM_BASE + 8u * VMM_PAGE_SIZE;
	cr_assert(
		map_object(vm_space_kernel(), memory, 0u, 2u, fixed, 1u, 2u, VMM_PROT_READ | VMM_PROT_WRITE, &fixed_id, NULL));
	cr_assert_not(
		map_object(vm_space_kernel(), memory, 0u, 1u, fixed - VMM_PAGE_SIZE, 1u, 0u, VMM_PROT_READ, &first_id, NULL),
		"exact mapping overlapped a guard");
	cr_assert(map_object(vm_space_kernel(), memory, 2u, 1u, 0u, 1u, 1u, VMM_PROT_READ, &first_id, &first));
	cr_assert(map_object(vm_space_kernel(), memory, 3u, 1u, 0u, 8u, 0u, VMM_PROT_READ, &aligned_id, &aligned));
	cr_assert_eq((uintptr_t)aligned & (8u * VMM_PAGE_SIZE - 1u), 0u);
	cr_assert_eq(vm_space_mapping_count(vm_space_kernel()), 3u);
	for (size_t i = 0u; i < 3u; i++) cr_assert(vm_space_query_at(vm_space_kernel(), i, &info[i]));
	cr_assert_lt((uintptr_t)info[0].base, (uintptr_t)info[1].base);
	cr_assert_lt((uintptr_t)info[1].base, (uintptr_t)info[2].base);
	cr_assert_not(vm_space_query(vm_space_kernel(), fixed - VMM_PAGE_SIZE, &info[0]), "guard became usable");
	cr_assert(vm_space_unmap(vm_space_kernel(), fixed_id));
	cr_assert(vm_space_unmap(vm_space_kernel(), first_id));
	cr_assert(vm_space_unmap(vm_space_kernel(), aligned_id));
	memory_object_release(memory);
}

Test(vmm, huge_sparse_mapping_uses_constant_initial_metadata) {
	struct memory_object* memory;
	vmm_id_t              id;
	void*                 base;
	init_test_vmm(arena, sizeof(arena));
	size_t before = pmm_free_size();
	cr_assert(memory_object_create_owned(200000u, &memory));
	cr_assert(map_object(vm_space_kernel(), memory, 0u, 200000u, 0u, 1u, 0u, VMM_PROT_READ, &id, &base));
	cr_assert_eq(before - pmm_free_size(), 2u * VMM_PAGE_SIZE, "object plus vector should use two control pages");
	cr_assert_not(hal_paging_query(vm_space_hal(vm_space_kernel()), (uintptr_t)base, NULL));
	cr_assert(vm_space_unmap(vm_space_kernel(), id));
	memory_object_release(memory);
	cr_assert_eq(pmm_free_size(), before);
}

Test(vmm, owned_object_implicit_zero_and_sparse_write) {
	struct memory_object* memory;
	uint8_t               readback[32];
	uint8_t               value[3] = {1u, 2u, 3u};
	init_test_vmm(arena, sizeof(arena));
	size_t before = pmm_free_size();
	cr_assert(memory_object_create_owned(4096u, &memory));
	size_t after_control = pmm_free_size();
	cr_assert(memory_object_read(memory, 17u * VMM_PAGE_SIZE + 7u, readback, sizeof(readback)));
	for (size_t i = 0u; i < sizeof(readback); i++) cr_assert_eq(readback[i], 0u);
	cr_assert_eq(pmm_free_size(), after_control, "zero read materialized backing");
	cr_assert(memory_object_write(memory, 17u * VMM_PAGE_SIZE + VMM_PAGE_SIZE - 1u, value, sizeof(value)));
	cr_assert(memory_object_read(memory, 17u * VMM_PAGE_SIZE + VMM_PAGE_SIZE - 1u, readback, sizeof(value)));
	cr_assert_arr_eq(readback, value, sizeof(value));
	cr_assert_lt(pmm_free_size(), after_control);
	memory_object_release(memory);
	cr_assert_eq(pmm_free_size(), before);
}

Test(vmm, shared_object_faults_reuse_backing_and_survive_unmap) {
	struct address_space          a = {0}, b = {0};
	struct memory_object*         memory;
	vmm_id_t                      a_id, b_id, remap_id;
	void *                        a_base, *b_base, *remap_base;
	struct hal_paging_translation a_translation, b_translation, remap_translation;
	uint8_t                       written = 0x5au, readback = 0u;
	init_test_vmm(arena, sizeof(arena));
	cr_assert(vm_space_create_user(&a));
	cr_assert(vm_space_create_user(&b));
	cr_assert(memory_object_create_owned(2u, &memory));
	cr_assert(map_object(&a, memory, 0u, 2u, 0u, 1u, 0u, VMM_PROT_READ | VMM_PROT_WRITE, &a_id, &a_base));
	cr_assert(map_object(&b, memory, 0u, 2u, 0u, 1u, 0u, VMM_PROT_READ | VMM_PROT_WRITE, &b_id, &b_base));
	cr_assert_not(hal_paging_query(vm_space_hal(&a), (uintptr_t)a_base, NULL));
	cr_assert(vm_space_resolve_page_fault(&a, (uintptr_t)a_base, VMM_FAULT_ACCESS_WRITE));
	cr_assert(vm_space_resolve_page_fault(&b, (uintptr_t)b_base, VMM_FAULT_ACCESS_READ));
	cr_assert(hal_paging_query(vm_space_hal(&a), (uintptr_t)a_base, &a_translation));
	cr_assert(hal_paging_query(vm_space_hal(&b), (uintptr_t)b_base, &b_translation));
	cr_assert_eq(a_translation.physical_address, b_translation.physical_address);
	cr_assert(memory_object_write(memory, 0u, &written, 1u));
	cr_assert(memory_object_read(memory, 0u, &readback, 1u));
	cr_assert_eq(readback, written);
	cr_assert(vm_space_unmap(&a, a_id));
	cr_assert(hal_paging_query(vm_space_hal(&b), (uintptr_t)b_base, &b_translation));
	cr_assert(map_object(&a, memory, 0u, 2u, 0u, 1u, 0u, VMM_PROT_READ, &remap_id, &remap_base));
	cr_assert(vm_space_resolve_page_fault(&a, (uintptr_t)remap_base, VMM_FAULT_ACCESS_READ));
	cr_assert(hal_paging_query(vm_space_hal(&a), (uintptr_t)remap_base, &remap_translation));
	cr_assert_eq(remap_translation.physical_address, b_translation.physical_address);
	cr_assert(vm_space_unmap(&a, remap_id));
	vm_space_destroy(&a);
	cr_assert(hal_paging_query(vm_space_hal(&b), (uintptr_t)b_base, NULL));
	cr_assert(vm_space_unmap(&b, b_id));
	vm_space_destroy(&b);
	memory_object_release(memory);
}

Test(vmm, protect_updates_present_and_future_pages) {
	struct memory_object*         memory;
	vmm_id_t                      id;
	void*                         base;
	struct hal_paging_translation translation;
	init_test_vmm(arena, sizeof(arena));
	cr_assert(memory_object_create_owned(2u, &memory));
	cr_assert(map_object(vm_space_kernel(), memory, 0u, 2u, 0u, 1u, 0u, VMM_PROT_READ | VMM_PROT_WRITE, &id, &base));
	cr_assert(vm_space_resolve_page_fault(vm_space_kernel(), (uintptr_t)base, VMM_FAULT_ACCESS_WRITE));
	cr_assert(vm_space_protect(vm_space_kernel(), id, VMM_PROT_READ | VMM_PROT_EXEC));
	cr_assert(hal_paging_query(vm_space_hal(vm_space_kernel()), (uintptr_t)base, &translation));
	cr_assert_eq(translation.flags, HAL_PAGE_READ | HAL_PAGE_EXEC);
	cr_assert(vm_space_resolve_page_fault(vm_space_kernel(), (uintptr_t)base + VMM_PAGE_SIZE, VMM_FAULT_ACCESS_READ));
	cr_assert(hal_paging_query(vm_space_hal(vm_space_kernel()), (uintptr_t)base + VMM_PAGE_SIZE, &translation));
	cr_assert_eq(translation.flags, HAL_PAGE_READ | HAL_PAGE_EXEC);
	cr_assert(vm_space_unmap(vm_space_kernel(), id));
	cr_assert(memory_object_read(memory, 0u, &translation, sizeof(uint64_t)), "unmap discarded object contents");
	memory_object_release(memory);
}

Test(vmm, failed_pte_install_keeps_resolved_object_page) {
	struct memory_object* memory;
	vmm_id_t              id;
	void*                 base;
	uintptr_t             phys;
	init_test_vmm(arena, sizeof(arena));
	cr_assert(memory_object_create_owned(1u, &memory));
	cr_assert(map_object(vm_space_kernel(), memory, 0u, 1u, 0u, 1u, 0u, VMM_PROT_READ, &id, &base));
	mock_paging_fail_after(0u);
	cr_assert_not(vm_space_resolve_page_fault(vm_space_kernel(), (uintptr_t)base, VMM_FAULT_ACCESS_READ));
	cr_assert(memory_object_page_phys(memory, 0u, &phys), "failed PTE install rolled back valid object data");
	cr_assert(vm_space_unmap(vm_space_kernel(), id));
	memory_object_release(memory);
}

Test(vmm, external_object_has_direct_backing_and_never_owns_frames) {
	struct memory_object* memory;
	uintptr_t             phys;
	init_test_vmm(arena, sizeof(arena));
	size_t before = pmm_free_size();
	cr_assert(memory_object_create_external(0x400000u, 3u, &memory));
	cr_assert(memory_object_page_phys(memory, 2u, &phys));
	cr_assert_eq(phys, 0x400000u + 2u * VMM_PAGE_SIZE);
	cr_assert_eq(before - pmm_free_size(), VMM_PAGE_SIZE);
	memory_object_release(memory);
	cr_assert_eq(pmm_free_size(), before);
}

Test(vmm, constrained_object_reserves_contiguous_backing) {
	struct memory_object*                  memory;
	uintptr_t                              first;
	uintptr_t                              last;
	static _Alignas(VMM_PAGE_SIZE) uint8_t constrained_arena[KiB(192)];

	init_test_vmm(constrained_arena, sizeof(constrained_arena));
	const struct memory_create_params params = {
		.page_count  = 3u,
		.memory_type = MEMORY_TYPE_NORMAL,
		.constraints =
			{
						  .physical_min = (uintptr_t)constrained_arena,
						  .physical_max = (uintptr_t)constrained_arena + sizeof(constrained_arena),
						  .align_pages  = 2u,
						  .flags        = MEMORY_CONSTRAINT_CONTIGUOUS,
						  },
	};
	cr_assert(memory_object_create(&params, &memory));
	cr_assert_eq(memory_object_type(memory), MEMORY_OBJECT_CONTIGUOUS);
	cr_assert_eq(memory_object_memory_type(memory), MEMORY_TYPE_NORMAL);
	cr_assert(memory_object_page_phys(memory, 0u, &first));
	cr_assert(memory_object_page_phys(memory, 2u, &last));
	cr_assert_eq(last, first + 2u * VMM_PAGE_SIZE);
	cr_assert_eq(first & (2u * VMM_PAGE_SIZE - 1u), 0u);
	memory_object_release(memory);
}

Test(vmm, bounded_object_materializes_each_page_inside_the_window) {
	struct memory_object*                  memory;
	uintptr_t                              phys;
	static _Alignas(VMM_PAGE_SIZE) uint8_t bounded_arena[KiB(192)];

	init_test_vmm(bounded_arena, sizeof(bounded_arena));
	const struct memory_create_params params = {
		.page_count = 3u,
		.constraints =
			{
						  .physical_min = (uintptr_t)(bounded_arena + KiB(64)),
						  .physical_max = (uintptr_t)(bounded_arena + sizeof(bounded_arena)),
						  },
	};
	cr_assert(memory_object_create(&params, &memory));
	cr_assert_eq(memory_object_type(memory), MEMORY_OBJECT_OWNED);
	for (size_t page = 0u; page < params.page_count; page++) {
		cr_assert(memory_object_page_phys(memory, page, &phys), "constrained page was left lazy");
		cr_assert_geq(phys, params.constraints.physical_min);
		cr_assert_lt(phys, params.constraints.physical_max);
	}
	memory_object_release(memory);
}

Test(vmm, fixed_external_objects_are_exclusive_and_support_physical_zero) {
	struct memory_object*                  first;
	struct memory_object*                  second;
	struct memory_object*                  zero;
	uintptr_t                              phys = UINTPTR_MAX;
	static _Alignas(VMM_PAGE_SIZE) uint8_t fixed_arena[KiB(192)];

	init_test_vmm(fixed_arena, sizeof(fixed_arena));
	const struct memory_create_params fixed = {
		.page_count  = 2u,
		.memory_type = MEMORY_TYPE_DEVICE,
		.constraints = {.physical_address = 0x400000u, .flags = MEMORY_CONSTRAINT_FIXED},
	};
	cr_assert(memory_object_create(&fixed, &first));
	cr_assert_eq(memory_object_type(first), MEMORY_OBJECT_EXTERNAL);
	cr_assert_eq(memory_object_memory_type(first), MEMORY_TYPE_DEVICE);
	cr_assert_not(memory_object_create(&fixed, &second));
	memory_object_release(first);
	cr_assert(memory_object_create(&fixed, &second));
	memory_object_release(second);

	struct memory_create_params at_zero  = fixed;
	at_zero.page_count                   = 1u;
	at_zero.constraints.physical_address = 0u;
	cr_assert(memory_object_create(&at_zero, &zero));
	cr_assert(memory_object_page_phys(zero, 0u, &phys));
	cr_assert_eq(phys, 0u);
	memory_object_release(zero);
}
