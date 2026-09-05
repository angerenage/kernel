#include <base/vmm.h>

#include "test_support.h"

Test(elf_loader_segments, loads_file_bytes_zeros_bss_and_applies_final_permissions) {
	struct elf_test_image     image;
	struct kernel_boot_module module;
	struct kernel_elf_process loaded = {0};
	struct address_space*     space;
	struct vmm_info           info;
	uint8_t                   bytes[64];
	const uint64_t            file_offset = VMM_PAGE_SIZE;
	const uint64_t            vaddr       = MM_USER_VMM_BASE + 4u * (uint64_t)VMM_PAGE_SIZE;
	elf_test_init_environment();
	elf_test_image_init(&image, 1u);
	elf_test_header(&image)->entry = vaddr;
	elf_test_set_load(&image, 0u, file_offset, vaddr, 32u, sizeof(bytes), ELF_TEST_PF_R | ELF_TEST_PF_X);
	for (size_t i = 0u; i < 32u; i++) image.bytes[file_offset + i] = (uint8_t)(0x40u + i);
	module = elf_test_module(&image);
	cr_assert_eq(kernel_elf_load_process(&module, "valid", &loaded), KERNEL_ELF_LOAD_OK);
	cr_assert_not_null(loaded.process);
	cr_assert_eq(loaded.entry, (uintptr_t)vaddr);
	cr_assert_neq(loaded.heap_base, 0u);
	cr_assert_eq(loaded.heap_page_count, HEAP_DEFAULT_GROW_PAGES);
	space = process_address_space(loaded.process);
	cr_assert_eq(address_space_copy_from(space, (uintptr_t)vaddr, bytes, sizeof(bytes)), ADDRESS_TRANSFER_OK);
	for (size_t i = 0u; i < 32u; i++) cr_assert_eq(bytes[i], (uint8_t)(0x40u + i));
	for (size_t i = 32u; i < sizeof(bytes); i++) cr_assert_eq(bytes[i], 0u, "BSS byte %zu was not zero", i);
	cr_assert(vm_space_query(space, (uintptr_t)vaddr, &info));
	cr_assert_eq(info.prot, (vmm_prot_t)(VMM_PROT_READ | VMM_PROT_EXEC));
	cr_assert(vm_space_query(space, loaded.heap_base, &info));
	cr_assert_eq(info.page_count, HEAP_DEFAULT_GROW_PAGES);
	cr_assert_eq(info.prot, (vmm_prot_t)(VMM_PROT_READ | VMM_PROT_WRITE));
	elf_test_destroy_loaded(&loaded);
}

Test(elf_loader_segments, keeps_text_and_data_permissions_independent) {
	struct elf_test_image     image;
	struct kernel_boot_module module;
	struct kernel_elf_process loaded = {0};
	struct address_space*     space;
	struct vmm_info           info;
	const uint64_t            text_offset = VMM_PAGE_SIZE;
	const uint64_t            data_offset = 2u * (uint64_t)VMM_PAGE_SIZE;
	const uint64_t            text_vaddr  = MM_USER_VMM_BASE + 4u * (uint64_t)VMM_PAGE_SIZE;
	const uint64_t            data_vaddr  = MM_USER_VMM_BASE + 8u * (uint64_t)VMM_PAGE_SIZE;
	uint8_t                   text[16], data[16];
	elf_test_init_environment();
	elf_test_image_init(&image, 2u);
	elf_test_header(&image)->entry = text_vaddr;
	elf_test_set_load(&image, 0u, text_offset, text_vaddr, sizeof(text), sizeof(text), ELF_TEST_PF_R | ELF_TEST_PF_X);
	elf_test_set_load(&image, 1u, data_offset, data_vaddr, sizeof(data), sizeof(data), ELF_TEST_PF_R | ELF_TEST_PF_W);
	memset(image.bytes + text_offset, 0x71, sizeof(text));
	memset(image.bytes + data_offset, 0x52, sizeof(data));
	module = elf_test_module(&image);
	cr_assert_eq(kernel_elf_load_process(&module, "two-segments", &loaded), KERNEL_ELF_LOAD_OK);
	space = process_address_space(loaded.process);
	cr_assert(vm_space_query(space, (uintptr_t)text_vaddr, &info));
	cr_assert_eq(info.prot, (vmm_prot_t)(VMM_PROT_READ | VMM_PROT_EXEC));
	cr_assert(vm_space_query(space, (uintptr_t)data_vaddr, &info));
	cr_assert_eq(info.prot, (vmm_prot_t)(VMM_PROT_READ | VMM_PROT_WRITE));
	cr_assert_eq(address_space_copy_from(space, text_vaddr, text, sizeof(text)), ADDRESS_TRANSFER_OK);
	cr_assert_eq(address_space_copy_from(space, data_vaddr, data, sizeof(data)), ADDRESS_TRANSFER_OK);
	for (size_t i = 0u; i < sizeof(text); i++) cr_assert_eq(text[i], 0x71u);
	for (size_t i = 0u; i < sizeof(data); i++) cr_assert_eq(data[i], 0x52u);
	elf_test_destroy_loaded(&loaded);
}

Test(elf_loader_segments, segment_page_padding_does_not_expose_recycled_physical_contents) {
	struct elf_test_image     image;
	struct kernel_boot_module module;
	struct kernel_elf_process loaded = {0};
	struct address_space*     space;
	uint8_t                   first_byte = 0u, last_byte = 0u;
	const uint8_t             poison      = 0xd7u;
	const uint64_t            page_offset = 0x120u;
	const uint64_t            file_offset = VMM_PAGE_SIZE + page_offset;
	const uint64_t            page_base   = MM_USER_VMM_BASE + 8u * (uint64_t)VMM_PAGE_SIZE;
	const uint64_t            vaddr       = page_base + page_offset;
	elf_test_init_environment();
	elf_test_poison_recycled_pages(32u, poison);
	elf_test_image_init(&image, 1u);
	elf_test_header(&image)->entry = vaddr;
	elf_test_set_load(&image, 0u, file_offset, vaddr, 8u, 16u, ELF_TEST_PF_R | ELF_TEST_PF_X);
	memset(image.bytes + file_offset, 0x31, 8u);
	module = elf_test_module(&image);
	cr_assert_eq(kernel_elf_load_process(&module, "padding", &loaded), KERNEL_ELF_LOAD_OK);
	space = process_address_space(loaded.process);
	cr_assert_eq(address_space_copy_from(space, page_base, &first_byte, 1u), ADDRESS_TRANSFER_OK);
	cr_assert_eq(address_space_copy_from(space, page_base + VMM_PAGE_SIZE - 1u, &last_byte, 1u), ADDRESS_TRANSFER_OK);
	cr_assert_neq(first_byte, poison, "leading PT_LOAD page padding exposed recycled physical data");
	cr_assert_neq(last_byte, poison, "trailing PT_LOAD page padding exposed recycled physical data");
	elf_test_destroy_loaded(&loaded);
}

Test(elf_loader_segments, initial_heap_does_not_expose_recycled_physical_contents) {
	struct elf_test_image     image;
	struct kernel_boot_module module;
	struct kernel_elf_process loaded = {0};
	struct address_space*     space;
	uint8_t                   first_byte = 0u, last_byte = 0u;
	const uint8_t             poison      = 0xa6u;
	const uint64_t            file_offset = VMM_PAGE_SIZE;
	const uint64_t            vaddr       = MM_USER_VMM_BASE + 4u * (uint64_t)VMM_PAGE_SIZE;
	uintptr_t                 heap_last;
	elf_test_init_environment();
	elf_test_poison_recycled_pages(32u, poison);
	elf_test_image_init(&image, 1u);
	elf_test_header(&image)->entry = vaddr;
	elf_test_set_load(&image, 0u, file_offset, vaddr, 16u, 16u, ELF_TEST_PF_R | ELF_TEST_PF_X);
	memset(image.bytes + file_offset, 0x19, 16u);
	module = elf_test_module(&image);
	cr_assert_eq(kernel_elf_load_process(&module, "heap-sanitize", &loaded), KERNEL_ELF_LOAD_OK);
	space     = process_address_space(loaded.process);
	heap_last = loaded.heap_base + loaded.heap_page_count * (uintptr_t)VMM_PAGE_SIZE - 1u;
	cr_assert_eq(address_space_copy_from(space, loaded.heap_base, &first_byte, 1u), ADDRESS_TRANSFER_OK);
	cr_assert_eq(address_space_copy_from(space, heap_last, &last_byte, 1u), ADDRESS_TRANSFER_OK);
	cr_assert_neq(first_byte, poison, "initial heap exposed recycled physical data at its first byte");
	cr_assert_neq(last_byte, poison, "initial heap exposed recycled physical data at its last byte");
	elf_test_destroy_loaded(&loaded);
}
