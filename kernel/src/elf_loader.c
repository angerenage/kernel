#include <base/heap.h>
#include <base/math.h>
#include <core/address_space.h>
#include <core/address_transfer.h>
#include <core/memory.h>
#include <core/mm.h>
#include <core/pmm.h>
#include <core/process.h>
#include <kernel/boot.h>
#include <kernel/elf_loader.h>
#include <libc/stdlib.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "capability/memory.h"

#define ELF_MAGIC0 0x7fu
#define ELF_MAGIC1 'E'
#define ELF_MAGIC2 'L'
#define ELF_MAGIC3 'F'
#define ELF_CLASS_64 2u
#define ELF_DATA_LSB 1u
#define ELF_VERSION_CURRENT 1u
#define ELF_ET_EXEC 2u
#define ELF_PT_LOAD 1u
#define ELF_PF_X 1u
#define ELF_PF_W 2u
#define ELF_PF_R 4u

#if defined(PLATFORM_PC_X86_64)
#define KERNEL_ELF_MACHINE 62u
#elif defined(PLATFORM_PC_AARCH64)
#define KERNEL_ELF_MACHINE 183u
#elif defined(PLATFORM_PC_RISCV64)
#define KERNEL_ELF_MACHINE 243u
#elif defined(PLATFORM_PC_LOONGARCH64)
#define KERNEL_ELF_MACHINE 258u
#else
#define KERNEL_ELF_MACHINE 0u
#endif

struct elf64_ehdr {
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

struct elf64_phdr {
	uint32_t type;
	uint32_t flags;
	uint64_t offset;
	uint64_t vaddr;
	uint64_t paddr;
	uint64_t filesz;
	uint64_t memsz;
	uint64_t align;
};

static bool kernel_elf_range_in_module(size_t module_size, uint64_t offset, uint64_t size) {
	uint64_t end;

	if (offset > (uint64_t)SIZE_MAX || size > (uint64_t)SIZE_MAX) return false;
	if (add_overflow_u64(offset, size, &end)) return false;
	return end <= (uint64_t)module_size;
}

static bool kernel_elf_mapping_range(uint64_t vaddr, uint64_t size, size_t granule, uintptr_t* out_base,
                                     size_t* out_size) {
	uint64_t page_base;
	uint64_t page_end;
	uint64_t end;

	if (out_base == NULL || out_size == NULL || granule == 0u || size == 0u) return false;
	if (add_overflow_u64(vaddr, size, &end)) return false;
	page_base = vaddr & ~(uint64_t)(granule - 1u);
	if (!align_up_u64(end, granule, &page_end)) return false;
	if (page_end <= page_base) return false;
	if (page_end - page_base > (uint64_t)SIZE_MAX) return false;

	*out_base = (uintptr_t)page_base;
	*out_size = (size_t)(page_end - page_base);
	return true;
}

static bool kernel_elf_header_valid(const struct elf64_ehdr* ehdr, size_t module_size) {
	uint64_t ph_size;

	if (ehdr == NULL) return false;
	if (ehdr->ident[0] != ELF_MAGIC0 || ehdr->ident[1] != ELF_MAGIC1 || ehdr->ident[2] != ELF_MAGIC2 ||
	    ehdr->ident[3] != ELF_MAGIC3) {
		return false;
	}
	if (ehdr->ident[4] != ELF_CLASS_64 || ehdr->ident[5] != ELF_DATA_LSB || ehdr->ident[6] != ELF_VERSION_CURRENT) {
		return false;
	}
	if (ehdr->type != ELF_ET_EXEC || ehdr->version != ELF_VERSION_CURRENT || ehdr->machine != KERNEL_ELF_MACHINE) {
		return false;
	}
	if (ehdr->ehsize != sizeof(*ehdr) || ehdr->phentsize != sizeof(struct elf64_phdr) || ehdr->phnum == 0u) {
		return false;
	}
	if (mul_overflow_u64((uint64_t)ehdr->phentsize, (uint64_t)ehdr->phnum, &ph_size)) return false;
	return kernel_elf_range_in_module(module_size, ehdr->phoff, ph_size);
}

static mapping_access_t kernel_elf_segment_access(uint32_t flags) {
	mapping_access_t access = 0u;

	if ((flags & ELF_PF_R) != 0) access |= MAPPING_ACCESS_READ;
	if ((flags & ELF_PF_W) != 0) access |= MAPPING_ACCESS_WRITE;
	if ((flags & ELF_PF_X) != 0) access |= MAPPING_ACCESS_EXEC;
	return access;
}

static cap_rights_t mapping_rights(mapping_access_t access) {
	cap_rights_t rights = CAP_CALL | CAP_READ | CAP_MAP | CAP_DESTROY | CAP_DELEGATE;
	if ((access & MAPPING_ACCESS_READ) != 0u) rights |= CAP_READ;
	if ((access & MAPPING_ACCESS_WRITE) != 0u) rights |= CAP_WRITE;
	if ((access & MAPPING_ACCESS_EXEC) != 0u) rights |= CAP_EXEC;
	return rights;
}

static enum kernel_elf_load_result kernel_elf_load_segment(struct process*                  process,
                                                           const struct kernel_boot_module* module,
                                                           const struct elf64_phdr*         phdr) {
	struct address_space* space;
	uintptr_t             map_base;
	size_t                mapping_size;
	size_t                granule      = address_space_minimum_mapping_size();
	struct mapping*       mapping      = NULL;
	struct memory*        memory       = NULL;
	mapping_access_t      final_access = kernel_elf_segment_access(phdr->flags);
	mapping_access_t      load_access  = final_access | MAPPING_ACCESS_READ | MAPPING_ACCESS_WRITE;

	if (phdr->filesz > phdr->memsz) return KERNEL_ELF_LOAD_BAD_FORMAT;
	if (phdr->memsz == 0u) return KERNEL_ELF_LOAD_OK;
	if (!kernel_elf_range_in_module(module->size, phdr->offset, phdr->filesz)) return KERNEL_ELF_LOAD_BAD_FORMAT;
	if (!kernel_elf_mapping_range(phdr->vaddr, phdr->memsz, granule, &map_base, &mapping_size)) {
		return KERNEL_ELF_LOAD_BAD_FORMAT;
	}
	if ((phdr->align != 0u && phdr->align != 1u && phdr->align != granule) ||
	    ((phdr->vaddr - phdr->offset) & (uint64_t)(granule - 1u)) != 0u) {
		return KERNEL_ELF_LOAD_UNSUPPORTED;
	}

	space = process_address_space(process);
	if (!memory_create_anonymous(mapping_size, &memory)) return KERNEL_ELF_LOAD_MAP_FAILED;
	if (!address_space_map(space,
	                       &(const struct address_space_mapping_request){
							   .memory  = memory,
							   .address = map_base,
							   .access  = load_access,
						   },
	                       &mapping)) {
		memory_release(memory);
		return KERNEL_ELF_LOAD_MAP_FAILED;
	}

	if (phdr->filesz != 0u && address_space_copy_to(space,
	                                                (uintptr_t)phdr->vaddr,
	                                                (const uint8_t*)module->address + (size_t)phdr->offset,
	                                                (size_t)phdr->filesz) != ADDRESS_TRANSFER_OK) {
		memory_release(memory);
		mapping_release(mapping);
		return KERNEL_ELF_LOAD_COPY_FAILED;
	}
	memory_release(memory);
	if (final_access != load_access && !address_space_protect(space, mapping, final_access)) {
		mapping_release(mapping);
		return KERNEL_ELF_LOAD_MAP_FAILED;
	}
	if (kernel_mapping_grant(process, process_pid(process), mapping, mapping_rights(final_access), final_access) ==
	    CAP_ID_INVALID) {
		(void)address_space_unmap(space, mapping);
		mapping_release(mapping);
		return KERNEL_ELF_LOAD_MAP_FAILED;
	}
	mapping_release(mapping);
	return KERNEL_ELF_LOAD_OK;
}

static enum kernel_elf_load_result kernel_elf_allocate_initial_heap(struct process* process, uintptr_t* out_base,
                                                                    size_t* out_size) {
	struct mapping* mapping;
	struct memory*  memory;

	if (process == NULL || out_base == NULL || out_size == NULL) return KERNEL_ELF_LOAD_INVALID_ARGUMENTS;
	size_t granule = address_space_minimum_mapping_size();
	size_t size;
	if (granule == 0u || !align_up_size(HEAP_DEFAULT_GROW_SIZE, granule, &size) ||
	    !memory_create_anonymous(size, &memory))
		return KERNEL_ELF_LOAD_MAP_FAILED;
	bool mapped = address_space_map(process_address_space(process),
	                                &(const struct address_space_mapping_request){
										.memory = memory,
										.access = MAPPING_ACCESS_READ | MAPPING_ACCESS_WRITE,
									},
	                                &mapping);
	memory_release(memory);
	if (!mapped) {
		return KERNEL_ELF_LOAD_MAP_FAILED;
	}
	*out_base = mapping_address(mapping);
	*out_size = size;
	if (kernel_mapping_grant(process,
	                         process_pid(process),
	                         mapping,
	                         mapping_rights(MAPPING_ACCESS_READ | MAPPING_ACCESS_WRITE),
	                         MAPPING_ACCESS_READ | MAPPING_ACCESS_WRITE) == CAP_ID_INVALID) {
		(void)address_space_unmap(process_address_space(process), mapping);
		mapping_release(mapping);
		return KERNEL_ELF_LOAD_MAP_FAILED;
	}
	mapping_release(mapping);
	return KERNEL_ELF_LOAD_OK;
}

enum kernel_elf_load_result kernel_elf_load_process(const struct kernel_boot_module* module, const char* name,
                                                    struct kernel_elf_process* out_process) {
	struct elf64_ehdr   ehdr;
	struct process*     process = NULL;
	enum process_result create_result;
	uintptr_t           heap_base = 0u;
	bool                saw_load  = false;

	if (out_process != NULL) *out_process = (struct kernel_elf_process){0};
	if (module == NULL || module->address == NULL || out_process == NULL) return KERNEL_ELF_LOAD_INVALID_ARGUMENTS;
	if (module->size < sizeof(struct elf64_ehdr)) return KERNEL_ELF_LOAD_BAD_FORMAT;

	memcpy(&ehdr, module->address, sizeof(ehdr));
	if (!kernel_elf_header_valid(&ehdr, module->size)) return KERNEL_ELF_LOAD_BAD_FORMAT;

	create_result = process_create(&process, name);
	if (create_result != PROCESS_OK)
		return create_result == PROCESS_NO_MEMORY ? KERNEL_ELF_LOAD_NO_MEMORY : KERNEL_ELF_LOAD_MAP_FAILED;

	for (size_t i = 0; i < ehdr.phnum; i++) {
		enum kernel_elf_load_result segment_result;
		struct elf64_phdr           phdr;
		size_t                      phdr_offset = (size_t)ehdr.phoff + i * sizeof(phdr);

		memcpy(&phdr, (const uint8_t*)module->address + phdr_offset, sizeof(phdr));
		if (phdr.type != ELF_PT_LOAD) continue;
		saw_load       = true;
		segment_result = kernel_elf_load_segment(process, module, &phdr);
		if (segment_result != KERNEL_ELF_LOAD_OK) {
			(void)process_destroy(process);
			return segment_result;
		}
	}
	if (!saw_load) {
		(void)process_destroy(process);
		return KERNEL_ELF_LOAD_BAD_FORMAT;
	}
	if (address_space_validate_range(
			process_address_space(process), (uintptr_t)ehdr.entry, 1u, ADDRESS_TRANSFER_EXEC | ADDRESS_TRANSFER_USER) !=
	    ADDRESS_TRANSFER_OK) {
		(void)process_destroy(process);
		return KERNEL_ELF_LOAD_BAD_FORMAT;
	}
	size_t                      heap_size;
	enum kernel_elf_load_result heap_result = kernel_elf_allocate_initial_heap(process, &heap_base, &heap_size);
	if (heap_result != KERNEL_ELF_LOAD_OK) {
		(void)process_destroy(process);
		return heap_result;
	}

	*out_process = (struct kernel_elf_process){
		.process   = process,
		.entry     = (uintptr_t)ehdr.entry,
		.heap_base = heap_base,
		.heap_size = heap_size,
	};
	return KERNEL_ELF_LOAD_OK;
}
