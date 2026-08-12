#ifndef TEST_ELF_LOADER_TEST_SUPPORT_H
#define TEST_ELF_LOADER_TEST_SUPPORT_H

#include <base/heap.h>
#include <core/address_transfer.h>
#include <core/cpu.h>
#include <core/mm.h>
#include <core/pmm.h>
#include <core/process.h>
#include <core/vmm.h>
#include <criterion/criterion.h>
#include <hal/cpu.h>
#include <hal/interrupts.h>
#include <kernel/boot.h>
#include <kernel/elf_loader.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../vmm/test_support.h"

#define ELF_TEST_IMAGE_CAPACITY (3u * PMM_PAGE_SIZE)
#define ELF_TEST_MAX_PHDRS 4u

#define ELF_TEST_PT_LOAD 1u
#define ELF_TEST_PF_X 1u
#define ELF_TEST_PF_W 2u
#define ELF_TEST_PF_R 4u

struct elf_test_ehdr {
	uint8_t  ident[16];
	uint16_t type;
	uint16_t machine;
	uint32_t version;
	uint64_t entry;
	uint64_t phoff;
	uint64_t shoff;
	uint32_t flags;
	uint16_t ehsize;
	uint16_t phentsize;
	uint16_t phnum;
	uint16_t shentsize;
	uint16_t shnum;
	uint16_t shstrndx;
};

struct elf_test_phdr {
	uint32_t type;
	uint32_t flags;
	uint64_t offset;
	uint64_t vaddr;
	uint64_t paddr;
	uint64_t filesz;
	uint64_t memsz;
	uint64_t align;
};

struct elf_test_image {
	_Alignas(16) uint8_t bytes[ELF_TEST_IMAGE_CAPACITY];
	size_t size;
};

void                      elf_test_init_environment(void);
void                      elf_test_image_init(struct elf_test_image* image, size_t phnum);
struct elf_test_ehdr*     elf_test_header(struct elf_test_image* image);
struct elf_test_phdr*     elf_test_phdr(struct elf_test_image* image, size_t index);
struct kernel_boot_module elf_test_module(struct elf_test_image* image);
void elf_test_set_load(struct elf_test_image* image, size_t index, uint64_t offset, uint64_t vaddr, uint64_t filesz,
                       uint64_t memsz, uint32_t flags);
void elf_test_destroy_loaded(struct kernel_elf_process* loaded);
void elf_test_poison_recycled_pages(size_t page_count, uint8_t value);

#endif
