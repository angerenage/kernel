#include "test_support.h"

#include <hal/cache.h>

#define KiB(x) ((size_t)(x) * 1024u)
#define ELF_TEST_ARENA_SIZE KiB(2048)
#define ELF_TEST_HEAP_SIZE KiB(256)

#define ELF_TEST_MAGIC0 0x7fu
#define ELF_TEST_MAGIC1 'E'
#define ELF_TEST_MAGIC2 'L'
#define ELF_TEST_MAGIC3 'F'
#define ELF_TEST_CLASS_64 2u
#define ELF_TEST_DATA_LSB 1u
#define ELF_TEST_VERSION_CURRENT 1u
#define ELF_TEST_ET_EXEC 2u

#if defined(PLATFORM_PC_X86_64)
#define ELF_TEST_MACHINE 62u
#elif defined(PLATFORM_PC_AARCH64)
#define ELF_TEST_MACHINE 183u
#elif defined(PLATFORM_PC_RISCV64)
#define ELF_TEST_MACHINE 243u
#elif defined(PLATFORM_PC_LOONGARCH64)
#define ELF_TEST_MACHINE 258u
#else
#define ELF_TEST_MACHINE 0u
#endif

static uint8_t elf_test_arena[ELF_TEST_ARENA_SIZE] __attribute__((aligned(PMM_PAGE_SIZE)));
static uint8_t elf_test_heap[ELF_TEST_HEAP_SIZE] __attribute__((aligned(PMM_PAGE_SIZE)));
static size_t  elf_test_heap_offset;

bool heap_grow_pages(size_t page_count, void** out_base) {
	size_t bytes;
	size_t offset;

	if (out_base == NULL) return false;
	*out_base = NULL;
	bytes     = page_count * PMM_PAGE_SIZE;
	for (;;) {
		offset = __atomic_load_n(&elf_test_heap_offset, __ATOMIC_ACQUIRE);
		if (bytes > ELF_TEST_HEAP_SIZE - offset) return false;
		if (__atomic_compare_exchange_n(
				&elf_test_heap_offset, &offset, offset + bytes, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
			*out_base = elf_test_heap + offset;
			return true;
		}
	}
}

void hal_cache_sync_executable_range(void* address, size_t size) {
	(void)address;
	(void)size;
}

void hal_cache_sync_executable_range_all_cpus(void* address, size_t size) {
	hal_cache_sync_executable_range(address, size);
}

void elf_test_init_environment(void) {
	const struct mem_range memory_map[] = {
		{.base = (uintptr_t)elf_test_arena, .length = ELF_TEST_ARENA_SIZE, .type = MEM_RANGE_USABLE},
	};

	elf_test_heap_offset = 0u;
	hal_cpu_local_bind(NULL);
	irq_enable_local();
	cr_assert(cpu_topology_init_bootstrap(0x100000u, 0x104000u));
	cr_assert_not_null(cpu_bsp());
	cpu_bind_current(cpu_bsp());
	cr_assert(cpu_set_state(cpu_current(), CPU_STATE_ONLINE));
	cpu_interrupts_set_ready(cpu_current(), false);
	mock_paging_reset();
	cr_assert(pmm_init(memory_map, sizeof(memory_map) / sizeof(memory_map[0]), 0u));
	cr_assert(vm_init());
	cr_assert(heap_init());
}

void elf_test_image_init(struct elf_test_image* image, size_t phnum) {
	struct elf_test_ehdr* header;

	cr_assert_not_null(image);
	cr_assert(phnum <= ELF_TEST_MAX_PHDRS);
	memset(image, 0, sizeof(*image));
	image->size = sizeof(image->bytes);
	header      = (struct elf_test_ehdr*)image->bytes;
	*header     = (struct elf_test_ehdr){
			.ident     = {ELF_TEST_MAGIC0,
	                      ELF_TEST_MAGIC1, ELF_TEST_MAGIC2,
	                      ELF_TEST_MAGIC3, ELF_TEST_CLASS_64,
	                      ELF_TEST_DATA_LSB, ELF_TEST_VERSION_CURRENT},
			.type      = ELF_TEST_ET_EXEC,
			.machine   = ELF_TEST_MACHINE,
			.version   = ELF_TEST_VERSION_CURRENT,
			.phoff     = sizeof(struct elf_test_ehdr),
			.ehsize    = sizeof(struct elf_test_ehdr),
			.phentsize = sizeof(struct elf_test_phdr),
			.phnum     = (uint16_t)phnum,
    };
}

struct elf_test_ehdr* elf_test_header(struct elf_test_image* image) {
	return image == NULL ? NULL : (struct elf_test_ehdr*)image->bytes;
}

struct elf_test_phdr* elf_test_phdr(struct elf_test_image* image, size_t index) {
	struct elf_test_ehdr* header = elf_test_header(image);
	if (header == NULL || index >= header->phnum) return NULL;
	return (struct elf_test_phdr*)(image->bytes + header->phoff + index * sizeof(struct elf_test_phdr));
}

struct kernel_boot_module elf_test_module(struct elf_test_image* image) {
	return (struct kernel_boot_module){
		.path    = "/boot/test.elf",
		.name    = "test.elf",
		.address = image == NULL ? NULL : image->bytes,
		.size    = image == NULL ? 0u : image->size,
	};
}

void elf_test_set_load(struct elf_test_image* image, size_t index, uint64_t offset, uint64_t vaddr, uint64_t filesz,
                       uint64_t memsz, uint32_t flags) {
	struct elf_test_phdr* phdr = elf_test_phdr(image, index);
	cr_assert_not_null(phdr);
	*phdr = (struct elf_test_phdr){
		.type   = ELF_TEST_PT_LOAD,
		.flags  = flags,
		.offset = offset,
		.vaddr  = vaddr,
		.filesz = filesz,
		.memsz  = memsz,
		.align  = PMM_PAGE_SIZE,
	};
}

void elf_test_destroy_loaded(struct kernel_elf_process* loaded) {
	if (loaded == NULL || loaded->process == NULL) return;
	cr_assert(process_destroy(loaded->process), "process_destroy failed for loaded ELF process");
	*loaded = (struct kernel_elf_process){0};
}

void elf_test_poison_recycled_pages(size_t page_count, uint8_t value) {
	uintptr_t phys = 0u;
	cr_assert(page_count != 0u);
	cr_assert(pmm_alloc_pages(page_count, &phys), "failed to reserve pages for recycle poisoning");
	memset((void*)(phys + boot_info.direct_map_offset), value, page_count * (size_t)PMM_PAGE_SIZE);
	cr_assert(pmm_free_pages(phys, page_count), "failed to return poisoned pages to PMM");
}
