#include "test_support.h"

Test(elf_loader_rollback, mapping_failure_reclaims_partial_process_and_backing) {
	struct elf_test_image       image;
	struct kernel_boot_module   module;
	struct kernel_elf_process   loaded = {0};
	enum kernel_elf_load_result result;
	size_t                      process_before, free_before;
	const uint64_t              text_offset = PMM_PAGE_SIZE;
	const uint64_t              data_offset = 2u * (uint64_t)PMM_PAGE_SIZE;
	const uint64_t              text_vaddr  = MM_USER_VMM_BASE + 4u * (uint64_t)PMM_PAGE_SIZE;
	const uint64_t              data_vaddr  = MM_USER_VMM_BASE + 8u * (uint64_t)PMM_PAGE_SIZE;
	elf_test_init_environment();
	elf_test_image_init(&image, 2u);
	elf_test_header(&image)->entry = text_vaddr;
	elf_test_set_load(&image, 0u, text_offset, text_vaddr, 16u, 16u, ELF_TEST_PF_R | ELF_TEST_PF_W | ELF_TEST_PF_X);
	elf_test_set_load(&image, 1u, data_offset, data_vaddr, 16u, 16u, ELF_TEST_PF_R | ELF_TEST_PF_W);
	memset(image.bytes + text_offset, 0x61, 16u);
	memset(image.bytes + data_offset, 0x72, 16u);
	module         = elf_test_module(&image);
	process_before = process_count();
	free_before    = pmm_free_page_count();
	mock_paging_fail_once_after(1u);
	result = kernel_elf_load_process(&module, "map-failure", &loaded);
	cr_assert_eq(result, KERNEL_ELF_LOAD_MAP_FAILED);
	cr_assert_null(loaded.process);
	cr_assert_eq(process_count(), process_before, "failed ELF load leaked a process-table entry");
	cr_assert_eq(mock_paging_mapping_count(), 0u, "failed ELF load leaked a hardware mapping");
	cr_assert_eq(pmm_free_page_count(), free_before, "failed ELF load leaked physical backing");
}

Test(elf_loader_rollback, invalid_entry_after_segment_load_reclaims_everything) {
	struct elf_test_image     image;
	struct kernel_boot_module module;
	struct kernel_elf_process loaded = {0};
	size_t                    process_before, free_before;
	const uint64_t            data_offset = PMM_PAGE_SIZE;
	const uint64_t            data_vaddr  = MM_USER_VMM_BASE + 4u * (uint64_t)PMM_PAGE_SIZE;
	elf_test_init_environment();
	elf_test_image_init(&image, 1u);
	elf_test_header(&image)->entry = data_vaddr;
	elf_test_set_load(&image, 0u, data_offset, data_vaddr, 32u, 64u, ELF_TEST_PF_R | ELF_TEST_PF_W);
	memset(image.bytes + data_offset, 0x44, 32u);
	module         = elf_test_module(&image);
	process_before = process_count();
	free_before    = pmm_free_page_count();
	cr_assert_eq(kernel_elf_load_process(&module, "invalid-entry", &loaded), KERNEL_ELF_LOAD_BAD_FORMAT);
	cr_assert_null(loaded.process);
	cr_assert_eq(process_count(), process_before);
	cr_assert_eq(mock_paging_mapping_count(), 0u);
	cr_assert_eq(pmm_free_page_count(), free_before);
}

Test(elf_loader_rollback, missing_load_segments_do_not_leave_an_empty_process) {
	struct elf_test_image     image;
	struct kernel_boot_module module;
	struct kernel_elf_process loaded = {0};
	size_t                    process_before, free_before;
	elf_test_init_environment();
	elf_test_image_init(&image, 1u);
	elf_test_header(&image)->entry  = MM_USER_VMM_BASE + 4u * (uintptr_t)PMM_PAGE_SIZE;
	elf_test_phdr(&image, 0u)->type = 0u;
	module                          = elf_test_module(&image);
	process_before                  = process_count();
	free_before                     = pmm_free_page_count();
	cr_assert_eq(kernel_elf_load_process(&module, "no-load", &loaded), KERNEL_ELF_LOAD_BAD_FORMAT);
	cr_assert_null(loaded.process);
	cr_assert_eq(process_count(), process_before);
	cr_assert_eq(mock_paging_mapping_count(), 0u);
	cr_assert_eq(pmm_free_page_count(), free_before);
}
