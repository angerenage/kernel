#include <base/vmm.h>

#include "test_support.h"

Test(elf_loader_rollback, invalid_entry_after_segment_load_reclaims_everything) {
	struct elf_test_image     image;
	struct kernel_boot_module module;
	struct kernel_elf_process loaded = {0};
	size_t                    process_before, free_before;
	const uint64_t            data_offset = VMM_PAGE_SIZE;
	const uint64_t            data_vaddr  = MM_USER_VMM_BASE + 4u * (uint64_t)VMM_PAGE_SIZE;
	elf_test_init_environment();
	elf_test_image_init(&image, 1u);
	elf_test_header(&image)->entry = data_vaddr;
	elf_test_set_load(&image, 0u, data_offset, data_vaddr, 32u, 64u, ELF_TEST_PF_R | ELF_TEST_PF_W);
	memset(image.bytes + data_offset, 0x44, 32u);
	module         = elf_test_module(&image);
	process_before = process_count();
	free_before    = pmm_free_size();
	cr_assert_eq(kernel_elf_load_process(&module, "invalid-entry", &loaded), KERNEL_ELF_LOAD_BAD_FORMAT);
	cr_assert_null(loaded.process);
	cr_assert_eq(process_count(), process_before);
	cr_assert_eq(mock_paging_mapping_count(), 0u);
	cr_assert_eq(pmm_free_size(), free_before);
}

Test(elf_loader_rollback, missing_load_segments_do_not_leave_an_empty_process) {
	struct elf_test_image     image;
	struct kernel_boot_module module;
	struct kernel_elf_process loaded = {0};
	size_t                    process_before, free_before;
	elf_test_init_environment();
	elf_test_image_init(&image, 1u);
	elf_test_header(&image)->entry  = MM_USER_VMM_BASE + 4u * (uintptr_t)VMM_PAGE_SIZE;
	elf_test_phdr(&image, 0u)->type = 0u;
	module                          = elf_test_module(&image);
	process_before                  = process_count();
	free_before                     = pmm_free_size();
	cr_assert_eq(kernel_elf_load_process(&module, "no-load", &loaded), KERNEL_ELF_LOAD_BAD_FORMAT);
	cr_assert_null(loaded.process);
	cr_assert_eq(process_count(), process_before);
	cr_assert_eq(mock_paging_mapping_count(), 0u);
	cr_assert_eq(pmm_free_size(), free_before);
}
