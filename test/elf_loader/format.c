#include <base/vmm.h>

#include "test_support.h"

static void make_minimal_exec(struct elf_test_image* image) {
	const uint64_t file_offset = VMM_PAGE_SIZE;
	const uint64_t vaddr       = MM_USER_VMM_BASE + 4u * (uint64_t)VMM_PAGE_SIZE;
	elf_test_image_init(image, 1u);
	elf_test_header(image)->entry = vaddr;
	elf_test_set_load(image, 0u, file_offset, vaddr, 16u, 16u, ELF_TEST_PF_R | ELF_TEST_PF_X);
	memset(image->bytes + file_offset, 0x41, 16u);
}

Test(elf_loader_format, rejects_invalid_top_level_arguments) {
	struct elf_test_image     image;
	struct kernel_boot_module module;
	struct kernel_elf_process loaded = {.process = (struct process*)(uintptr_t)1u, .entry = 1u};

	elf_test_init_environment();
	make_minimal_exec(&image);
	module = elf_test_module(&image);
	cr_assert_eq(kernel_elf_load_process(NULL, "bad", &loaded), KERNEL_ELF_LOAD_INVALID_ARGUMENTS);
	cr_assert_null(loaded.process);
	cr_assert_eq(loaded.entry, 0u);
	module.address = NULL;
	cr_assert_eq(kernel_elf_load_process(&module, "bad", &loaded), KERNEL_ELF_LOAD_INVALID_ARGUMENTS);
	cr_assert_null(loaded.process);
	module = elf_test_module(&image);
	cr_assert_eq(kernel_elf_load_process(&module, "bad", NULL), KERNEL_ELF_LOAD_INVALID_ARGUMENTS);
}

Test(elf_loader_format, rejects_truncated_or_incompatible_headers) {
	struct elf_test_image     image;
	struct kernel_boot_module module;
	struct kernel_elf_process loaded;

	elf_test_init_environment();
	make_minimal_exec(&image);
	module      = elf_test_module(&image);
	module.size = sizeof(struct elf_test_ehdr) - 1u;
	cr_assert_eq(kernel_elf_load_process(&module, "bad", &loaded), KERNEL_ELF_LOAD_BAD_FORMAT);
	make_minimal_exec(&image);
	module = elf_test_module(&image);
	elf_test_header(&image)->ident[0] ^= 0xffu;
	cr_assert_eq(kernel_elf_load_process(&module, "bad", &loaded), KERNEL_ELF_LOAD_BAD_FORMAT);
	make_minimal_exec(&image);
	module                            = elf_test_module(&image);
	elf_test_header(&image)->ident[4] = 1u;
	cr_assert_eq(kernel_elf_load_process(&module, "bad", &loaded), KERNEL_ELF_LOAD_BAD_FORMAT);
	make_minimal_exec(&image);
	module = elf_test_module(&image);
	elf_test_header(&image)->machine ^= 1u;
	cr_assert_eq(kernel_elf_load_process(&module, "bad", &loaded), KERNEL_ELF_LOAD_BAD_FORMAT);
}

Test(elf_loader_format, rejects_program_header_table_outside_the_module) {
	struct elf_test_image     image;
	struct kernel_boot_module module;
	struct kernel_elf_process loaded;
	elf_test_init_environment();
	make_minimal_exec(&image);
	module      = elf_test_module(&image);
	module.size = sizeof(struct elf_test_ehdr) + sizeof(struct elf_test_phdr) - 1u;
	cr_assert_eq(kernel_elf_load_process(&module, "bad-phdr-table", &loaded), KERNEL_ELF_LOAD_BAD_FORMAT);
	cr_assert_null(loaded.process);
}

Test(elf_loader_format, rejects_filesz_larger_than_memsz_even_when_memsz_is_zero) {
	struct elf_test_image       image;
	struct kernel_boot_module   module;
	struct kernel_elf_process   loaded = {0};
	enum kernel_elf_load_result result;
	const uint64_t              exec_offset = VMM_PAGE_SIZE;
	const uint64_t              exec_vaddr  = MM_USER_VMM_BASE + 4u * (uint64_t)VMM_PAGE_SIZE;
	const uint64_t              bad_offset  = 2u * (uint64_t)VMM_PAGE_SIZE;
	const uint64_t              bad_vaddr   = MM_USER_VMM_BASE + 8u * (uint64_t)VMM_PAGE_SIZE;

	elf_test_init_environment();
	elf_test_image_init(&image, 2u);
	elf_test_header(&image)->entry = exec_vaddr;
	elf_test_set_load(&image, 0u, exec_offset, exec_vaddr, 16u, 16u, ELF_TEST_PF_R | ELF_TEST_PF_X);
	elf_test_set_load(&image, 1u, bad_offset, bad_vaddr, 1u, 0u, ELF_TEST_PF_R);
	image.bytes[exec_offset] = 0x90u;
	image.bytes[bad_offset]  = 0x5au;
	module                   = elf_test_module(&image);
	result                   = kernel_elf_load_process(&module, "filesz-gt-memsz", &loaded);
	if (result == KERNEL_ELF_LOAD_OK) elf_test_destroy_loaded(&loaded);
	cr_assert_eq(result,
	             KERNEL_ELF_LOAD_BAD_FORMAT,
	             "a PT_LOAD with filesz > memsz must invalidate the ELF even when memsz is zero");
}

Test(elf_loader_format, rejects_segment_file_ranges_outside_the_module) {
	struct elf_test_image     image;
	struct kernel_boot_module module;
	struct kernel_elf_process loaded;
	const uint64_t            vaddr = MM_USER_VMM_BASE + 4u * (uint64_t)VMM_PAGE_SIZE;
	elf_test_init_environment();
	elf_test_image_init(&image, 1u);
	elf_test_header(&image)->entry = vaddr;
	elf_test_set_load(
		&image, 0u, 2u * VMM_PAGE_SIZE, vaddr, VMM_PAGE_SIZE + 1u, VMM_PAGE_SIZE + 1u, ELF_TEST_PF_R | ELF_TEST_PF_X);
	module = elf_test_module(&image);
	cr_assert_eq(kernel_elf_load_process(&module, "bad-file-range", &loaded), KERNEL_ELF_LOAD_BAD_FORMAT);
	cr_assert_null(loaded.process);
}

Test(elf_loader_format, rejects_overflowing_virtual_segment_ranges) {
	struct elf_test_image     image;
	struct kernel_boot_module module;
	struct kernel_elf_process loaded;
	elf_test_init_environment();
	elf_test_image_init(&image, 1u);
	elf_test_header(&image)->entry = UINT64_MAX - 15u;
	elf_test_set_load(&image, 0u, VMM_PAGE_SIZE, UINT64_MAX - 15u, 8u, 32u, ELF_TEST_PF_R | ELF_TEST_PF_X);
	module = elf_test_module(&image);
	cr_assert_eq(kernel_elf_load_process(&module, "overflow", &loaded), KERNEL_ELF_LOAD_BAD_FORMAT);
	cr_assert_null(loaded.process);
}

Test(elf_loader_format, rejects_entry_points_without_executable_mapping) {
	struct elf_test_image     image;
	struct kernel_boot_module module;
	struct kernel_elf_process loaded;
	const uint64_t            vaddr = MM_USER_VMM_BASE + 4u * (uint64_t)VMM_PAGE_SIZE;
	elf_test_init_environment();
	elf_test_image_init(&image, 1u);
	elf_test_header(&image)->entry = vaddr;
	elf_test_set_load(&image, 0u, VMM_PAGE_SIZE, vaddr, 16u, 16u, ELF_TEST_PF_R | ELF_TEST_PF_W);
	memset(image.bytes + VMM_PAGE_SIZE, 0x24, 16u);
	module = elf_test_module(&image);
	cr_assert_eq(kernel_elf_load_process(&module, "nonexec-entry", &loaded), KERNEL_ELF_LOAD_BAD_FORMAT);
	cr_assert_null(loaded.process);
}

Test(elf_loader_format, handles_misaligned_module_storage_without_undefined_access) {
	struct elf_test_image       aligned;
	_Alignas(16) uint8_t        storage[ELF_TEST_IMAGE_CAPACITY + 1u];
	struct kernel_boot_module   module;
	struct kernel_elf_process   loaded = {0};
	enum kernel_elf_load_result result;
	elf_test_init_environment();
	make_minimal_exec(&aligned);
	memcpy(storage + 1u, aligned.bytes, aligned.size);
	module = (struct kernel_boot_module){
		.path = "/boot/misaligned.elf", .name = "misaligned.elf", .address = storage + 1u, .size = aligned.size};
	result = kernel_elf_load_process(&module, "misaligned", &loaded);
	cr_assert(result == KERNEL_ELF_LOAD_OK || result == KERNEL_ELF_LOAD_BAD_FORMAT ||
	              result == KERNEL_ELF_LOAD_UNSUPPORTED,
	          "misaligned byte storage must be handled as input, not through undefined typed loads");
	if (result == KERNEL_ELF_LOAD_OK) elf_test_destroy_loaded(&loaded);
}
