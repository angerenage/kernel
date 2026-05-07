#include <base/math.h>
#include <core/address_transfer.h>
#include <core/mm.h>
#include <core/pmm.h>
#include <core/process.h>
#include <core/vmm.h>
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
	vmm_prot_t prot = VMM_PROT_USER;

	if ((flags & ELF_PF_R) != 0) prot |= VMM_PROT_READ;
	if ((flags & ELF_PF_W) != 0) prot |= VMM_PROT_WRITE;
	if ((flags & ELF_PF_X) != 0) prot |= VMM_PROT_EXEC;
	return prot;
}

static enum kernel_elf_load_result kernel_elf_zero_range(struct address_space* space, uintptr_t addr, size_t size) {
	static const uint8_t zeroes[256];
	size_t               zeroed = 0u;

	while (zeroed < size) {
		size_t chunk = size - zeroed;

		if (chunk > sizeof(zeroes)) chunk = sizeof(zeroes);
		if (address_space_copy_to(space, addr + zeroed, zeroes, chunk) != ADDRESS_TRANSFER_OK) {
			return KERNEL_ELF_LOAD_COPY_FAILED;
		}
		zeroed += chunk;
	}
	return KERNEL_ELF_LOAD_OK;
}

static void kernel_elf_sync_loaded_pages(struct address_space* space, uintptr_t base, size_t page_count) {
	for (size_t page = 0; page < page_count; page++) {
		uintptr_t phys = 0u;
		char*     start;
		char*     end;

		if (!hal_paging_query(address_space_hal(space), base + page * (uintptr_t)PMM_PAGE_SIZE, &phys, NULL)) {
			continue;
		}

		start = (char*)(phys + boot_info.direct_map_offset);
		end   = start + PMM_PAGE_SIZE;
		hal_paging_sync_executable_range(start, (size_t)(end - start));
	}
}

static enum kernel_elf_load_result kernel_elf_load_segment(struct process*                  process,
                                                           const struct kernel_boot_module* module,
                                                           const struct elf64_phdr*         phdr) {
	struct address_space* space;
	uintptr_t             map_base;
	size_t                page_count;
	vmm_id_t              id         = VMM_ID_INVALID;
	vmm_prot_t            final_prot = kernel_elf_segment_prot(phdr->flags);
	vmm_prot_t            load_prot  = final_prot | VMM_PROT_READ | VMM_PROT_WRITE | VMM_PROT_USER;

	if (phdr->memsz == 0u) return KERNEL_ELF_LOAD_OK;
	if (phdr->filesz > phdr->memsz) return KERNEL_ELF_LOAD_BAD_FORMAT;
	if ((phdr->align != 0u && phdr->align != 1u && phdr->align != PMM_PAGE_SIZE) ||
	    ((phdr->vaddr - phdr->offset) & (uint64_t)(PMM_PAGE_SIZE - 1u)) != 0u) {
		return KERNEL_ELF_LOAD_UNSUPPORTED;
	}
	if (!kernel_elf_range_in_module(module->size, phdr->offset, phdr->filesz)) return KERNEL_ELF_LOAD_BAD_FORMAT;
	if (!kernel_elf_range_to_pages(phdr->vaddr, phdr->memsz, &map_base, &page_count)) {
		return KERNEL_ELF_LOAD_BAD_FORMAT;
	}

	space = process_address_space(process);
	if (!vmm_alloc_at(space,
	                  (void*)map_base,
	                  &(const struct vmm_alloc_params){
						  .page_count  = page_count,
						  .align_pages = 1u,
						  .prot        = load_prot,
						  .kind        = VMM_KIND_GENERIC,
					  },
	                  &id)) {
		return KERNEL_ELF_LOAD_MAP_FAILED;
	}

	if (phdr->filesz != 0u && address_space_copy_to(space,
	                                                (uintptr_t)phdr->vaddr,
	                                                (const uint8_t*)module->address + (size_t)phdr->offset,
	                                                (size_t)phdr->filesz) != ADDRESS_TRANSFER_OK) {
		return KERNEL_ELF_LOAD_COPY_FAILED;
	}
	if (phdr->memsz > phdr->filesz) {
		enum kernel_elf_load_result zero_result =
			kernel_elf_zero_range(space, (uintptr_t)(phdr->vaddr + phdr->filesz), (size_t)(phdr->memsz - phdr->filesz));
		if (zero_result != KERNEL_ELF_LOAD_OK) return zero_result;
	}
	if ((final_prot & VMM_PROT_EXEC) != 0) kernel_elf_sync_loaded_pages(space, map_base, page_count);
	if (final_prot != load_prot && !vmm_protect(space, id, final_prot)) return KERNEL_ELF_LOAD_MAP_FAILED;
	return KERNEL_ELF_LOAD_OK;
}

enum kernel_elf_load_result kernel_elf_load_process(const struct kernel_boot_module* module, const char* name,
                                                    struct kernel_elf_process* out_process) {
	const struct elf64_ehdr* ehdr;
	const struct elf64_phdr* phdrs;
	struct process*          process = NULL;
	enum process_result      create_result;
	bool                     saw_load = false;

	if (out_process != NULL) *out_process = (struct kernel_elf_process){0};
	if (module == NULL || module->address == NULL || out_process == NULL) return KERNEL_ELF_LOAD_INVALID_ARGUMENTS;
	if (module->size < sizeof(struct elf64_ehdr)) return KERNEL_ELF_LOAD_BAD_FORMAT;

	ehdr = (const struct elf64_ehdr*)module->address;
	if (!kernel_elf_header_valid(ehdr, module->size)) return KERNEL_ELF_LOAD_BAD_FORMAT;

	create_result = process_create(&process, name);
	if (create_result != PROCESS_OK)
		return create_result == PROCESS_NO_MEMORY ? KERNEL_ELF_LOAD_NO_MEMORY : KERNEL_ELF_LOAD_MAP_FAILED;

	phdrs = (const struct elf64_phdr*)((const uint8_t*)module->address + (size_t)ehdr->phoff);
	for (size_t i = 0; i < ehdr->phnum; i++) {
		enum kernel_elf_load_result segment_result;

		if (phdrs[i].type != ELF_PT_LOAD) continue;
		saw_load       = true;
		segment_result = kernel_elf_load_segment(process, module, &phdrs[i]);
		if (segment_result != KERNEL_ELF_LOAD_OK) {
			(void)process_destroy(process);
			return segment_result;
		}
	}
	if (!saw_load) {
		(void)process_destroy(process);
		return KERNEL_ELF_LOAD_BAD_FORMAT;
	}
	if (address_space_validate_range(process_address_space(process),
	                                 (uintptr_t)ehdr->entry,
	                                 1u,
	                                 ADDRESS_TRANSFER_EXEC | ADDRESS_TRANSFER_USER | ADDRESS_TRANSFER_PRESENT) !=
	    ADDRESS_TRANSFER_OK) {
		(void)process_destroy(process);
		return KERNEL_ELF_LOAD_BAD_FORMAT;
	}

	*out_process = (struct kernel_elf_process){
		.process = process,
		.entry   = (uintptr_t)ehdr->entry,
	};
	return KERNEL_ELF_LOAD_OK;
}
