#include <base/heap.h>
#include <base/math.h>
#include <core/address_transfer.h>
#include <core/memory_object.h>
#include <core/mm.h>
#include <core/pmm.h>
#include <core/process.h>
#include <core/vm_space.h>
#include <hal/paging.h>
#include <kernel/boot.h>
#include <kernel/elf_loader.h>
#include <libc/stdlib.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

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

static bool kernel_elf_range_to_pages(uint64_t vaddr, uint64_t size, uintptr_t* out_base, size_t* out_pages) {
	uint64_t page_base;
	uint64_t page_end;
	uint64_t end;

	if (out_base == NULL || out_pages == NULL || size == 0u) return false;
	if (add_overflow_u64(vaddr, size, &end)) return false;
	page_base = vaddr & ~(uint64_t)(PMM_PAGE_SIZE - 1u);
	if (!align_up_u64(end, PMM_PAGE_SIZE, &page_end)) return false;
	if (page_end <= page_base) return false;
	if (((page_end - page_base) / PMM_PAGE_SIZE) > (uint64_t)SIZE_MAX) return false;

	*out_base  = (uintptr_t)page_base;
	*out_pages = (size_t)((page_end - page_base) / PMM_PAGE_SIZE);
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

static vmm_prot_t kernel_elf_segment_prot(uint32_t flags) {
	vmm_prot_t prot = VMM_PROT_NONE;

	if ((flags & ELF_PF_R) != 0) prot |= VMM_PROT_READ;
	if ((flags & ELF_PF_W) != 0) prot |= VMM_PROT_WRITE;
	if ((flags & ELF_PF_X) != 0) prot |= VMM_PROT_EXEC;
	return prot;
}

static void kernel_elf_sync_loaded_pages(struct memory_object* memory, size_t page_count) {
	for (size_t page = 0; page < page_count; page++) {
		uintptr_t phys = 0u;
		if (memory_object_page_phys(memory, page, &phys))
			hal_paging_sync_executable_range((void*)(phys + boot_info.direct_map_offset), PMM_PAGE_SIZE);
	}
}

static enum kernel_elf_load_result kernel_elf_load_segment(struct process*                  process,
                                                           const struct kernel_boot_module* module,
                                                           const struct elf64_phdr*         phdr) {
	struct address_space* space;
	uintptr_t             map_base;
	size_t                page_count;
	vmm_id_t              id         = VMM_ID_INVALID;
	struct memory_object* memory     = NULL;
	vmm_prot_t            final_prot = kernel_elf_segment_prot(phdr->flags);
	vmm_prot_t            load_prot  = final_prot | VMM_PROT_READ | VMM_PROT_WRITE;

	if (phdr->filesz > phdr->memsz) return KERNEL_ELF_LOAD_BAD_FORMAT;
	if (phdr->memsz == 0u) return KERNEL_ELF_LOAD_OK;
	if (!kernel_elf_range_in_module(module->size, phdr->offset, phdr->filesz)) return KERNEL_ELF_LOAD_BAD_FORMAT;
	if (!kernel_elf_range_to_pages(phdr->vaddr, phdr->memsz, &map_base, &page_count)) {
		return KERNEL_ELF_LOAD_BAD_FORMAT;
	}
	if ((phdr->align != 0u && phdr->align != 1u && phdr->align != PMM_PAGE_SIZE) ||
	    ((phdr->vaddr - phdr->offset) & (uint64_t)(PMM_PAGE_SIZE - 1u)) != 0u) {
		return KERNEL_ELF_LOAD_UNSUPPORTED;
	}

	space = process_address_space(process);
	if (!memory_object_create_owned(page_count, &memory)) return KERNEL_ELF_LOAD_MAP_FAILED;
	if (!vm_space_map(space,
	                  &(const struct vm_map_request){
						  .memory         = memory,
						  .page_count     = page_count,
						  .requested_base = map_base,
						  .align_pages    = 1u,
						  .prot           = load_prot,
					  },
	                  &id,
	                  NULL)) {
		memory_object_release(memory);
		return KERNEL_ELF_LOAD_MAP_FAILED;
	}

	if (phdr->filesz != 0u && address_space_copy_to(space,
	                                                (uintptr_t)phdr->vaddr,
	                                                (const uint8_t*)module->address + (size_t)phdr->offset,
	                                                (size_t)phdr->filesz) != ADDRESS_TRANSFER_OK) {
		memory_object_release(memory);
		return KERNEL_ELF_LOAD_COPY_FAILED;
	}
	if ((final_prot & VMM_PROT_EXEC) != 0) kernel_elf_sync_loaded_pages(memory, page_count);
	memory_object_release(memory);
	if (final_prot != load_prot && !vm_space_protect(space, id, final_prot)) return KERNEL_ELF_LOAD_MAP_FAILED;
	return KERNEL_ELF_LOAD_OK;
}

static enum kernel_elf_load_result kernel_elf_allocate_initial_heap(struct process* process, uintptr_t* out_base) {
	void*                 base = NULL;
	vmm_id_t              id   = VMM_ID_INVALID;
	struct memory_object* memory;

	if (process == NULL || out_base == NULL) return KERNEL_ELF_LOAD_INVALID_ARGUMENTS;
	if (!memory_object_create_owned(HEAP_DEFAULT_GROW_PAGES, &memory)) return KERNEL_ELF_LOAD_MAP_FAILED;
	bool mapped = vm_space_map(process_address_space(process),
	                           &(const struct vm_map_request){
								   .memory      = memory,
								   .page_count  = HEAP_DEFAULT_GROW_PAGES,
								   .align_pages = 1u,
								   .prot        = VMM_PROT_READ | VMM_PROT_WRITE,
							   },
	                           &id,
	                           &base);
	memory_object_release(memory);
	if (!mapped) {
		return KERNEL_ELF_LOAD_MAP_FAILED;
	}
	*out_base = (uintptr_t)base;
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
	enum kernel_elf_load_result heap_result = kernel_elf_allocate_initial_heap(process, &heap_base);
	if (heap_result != KERNEL_ELF_LOAD_OK) {
		(void)process_destroy(process);
		return heap_result;
	}

	*out_process = (struct kernel_elf_process){
		.process         = process,
		.entry           = (uintptr_t)ehdr.entry,
		.heap_base       = heap_base,
		.heap_page_count = HEAP_DEFAULT_GROW_PAGES,
	};
	return KERNEL_ELF_LOAD_OK;
}
