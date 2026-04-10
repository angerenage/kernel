#include <hal/cpu.h>
#include <kernel/boot.h>
#include <stddef.h>
#include <stdint.h>

#include "limine_requests.h"

#define KERNEL_BOOT_MAX_CPUS 64u
#define KERNEL_BOOT_MAX_MEM_RANGES 256u

struct kernel_boot_cpu_launch {
	kernel_boot_cpu_entry_t entry;
	void*                   arg;
	size_t                  cpu_index;
};

static struct mem_range                 boot_memmap[KERNEL_BOOT_MAX_MEM_RANGES];
static size_t                           boot_memmap_count;
static struct kernel_boot_framebuffer   boot_framebuffer;
static bool                             boot_framebuffer_valid;
static struct kernel_boot_address_space boot_address_space;
static bool                             boot_address_space_valid;
static const char*                      boot_cmdline;
static uintptr_t                        boot_rsdp_address;
static bool                             boot_rsdp_valid;
static struct kernel_boot_cpu_launch    boot_cpu_launch[KERNEL_BOOT_MAX_CPUS];
static void*                            boot_cpu_private[KERNEL_BOOT_MAX_CPUS];
static size_t                           boot_cpu_count;
static bool                             boot_initialized;

static enum mem_range_type kernel_boot_mem_range_type(uint64_t type) {
	switch (type) {
	case LIMINE_MEMMAP_USABLE:
		return MEM_RANGE_USABLE;
	case LIMINE_MEMMAP_RESERVED:
		return MEM_RANGE_RESERVED;
	case LIMINE_MEMMAP_ACPI_RECLAIMABLE:
		return MEM_RANGE_ACPI_RECLAIMABLE;
	case LIMINE_MEMMAP_ACPI_NVS:
		return MEM_RANGE_ACPI_NVS;
	case LIMINE_MEMMAP_BAD_MEMORY:
		return MEM_RANGE_BAD_MEMORY;
	case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE:
		return MEM_RANGE_BOOTLOADER_RECLAIMABLE;
	case LIMINE_MEMMAP_KERNEL_AND_MODULES:
		return MEM_RANGE_KERNEL_AND_MODULES;
	case LIMINE_MEMMAP_FRAMEBUFFER:
		return MEM_RANGE_FRAMEBUFFER;
	default:
		return MEM_RANGE_OTHER;
	}
}

#if defined(PLATFORM_PC_X86_64)
static uint64_t kernel_boot_mp_info_arch_id(const struct LIMINE_MP(info) * info) {
	return info ? (uint64_t)info->lapic_id : 0u;
}

static uint64_t kernel_boot_mp_info_processor_id(const struct LIMINE_MP(info) * info) {
	return info ? (uint64_t)info->processor_id : 0u;
}

static uint64_t kernel_boot_mp_bsp_arch_id(const struct LIMINE_MP(response) * response) {
	return response ? (uint64_t)response->bsp_lapic_id : 0u;
}

static bool kernel_boot_mp_supported(void) {
	return true;
}
#elif defined(PLATFORM_PC_AARCH64)
static uint64_t kernel_boot_mp_info_arch_id(const struct LIMINE_MP(info) * info) {
	return info ? info->mpidr : 0u;
}

static uint64_t kernel_boot_mp_info_processor_id(const struct LIMINE_MP(info) * info) {
	return info ? (uint64_t)info->processor_id : 0u;
}

static uint64_t kernel_boot_mp_bsp_arch_id(const struct LIMINE_MP(response) * response) {
	return response ? response->bsp_mpidr : 0u;
}

static bool kernel_boot_mp_supported(void) {
	return true;
}
#elif defined(PLATFORM_PC_RISCV64)
static uint64_t kernel_boot_mp_info_arch_id(const struct LIMINE_MP(info) * info) {
	return info ? info->hartid : 0u;
}

static uint64_t kernel_boot_mp_info_processor_id(const struct LIMINE_MP(info) * info) {
	return info ? info->processor_id : 0u;
}

static uint64_t kernel_boot_mp_bsp_arch_id(const struct LIMINE_MP(response) * response) {
	return response ? response->bsp_hartid : 0u;
}

static bool kernel_boot_mp_supported(void) {
	return true;
}
#else
static uint64_t kernel_boot_mp_info_arch_id(const struct LIMINE_MP(info) * info) {
	(void)info;
	return 0u;
}

static uint64_t kernel_boot_mp_info_processor_id(const struct LIMINE_MP(info) * info) {
	(void)info;
	return 0u;
}

static uint64_t kernel_boot_mp_bsp_arch_id(const struct LIMINE_MP(response) * response) {
	(void)response;
	return 0u;
}

static bool kernel_boot_mp_supported(void) {
	return false;
}
#endif

#if defined(PLATFORM_PC_X86_64) || defined(PLATFORM_PC_AARCH64) || defined(PLATFORM_PC_RISCV64)
static void kernel_boot_mp_entry(struct LIMINE_MP(info) * info) {
	struct kernel_boot_cpu_launch* launch;

	if (info == NULL) {
		for (;;) {
			hal_cpu_park();
		}
	}

	launch = (struct kernel_boot_cpu_launch*)(uintptr_t)info->extra_argument;
	if (launch == NULL || launch->entry == NULL) {
		for (;;) {
			hal_cpu_park();
		}
	}

	launch->entry(launch->cpu_index, launch->arg);
	for (;;) {
		hal_cpu_park();
	}
}
#endif

bool kernel_boot_init(void) {
	if (boot_initialized) return true;
	if (!kernel_limine_protocol_supported()) return false;
	if (memmap_req.response == NULL || memmap_req.response->entries == NULL || memmap_req.response->entry_count == 0u)
		return false;
	if (memmap_req.response->entry_count > KERNEL_BOOT_MAX_MEM_RANGES) return false;
	if (hhdm_req.response == NULL || exec_addr_req.response == NULL) return false;

	boot_memmap_count = (size_t)memmap_req.response->entry_count;
	for (size_t i = 0; i < boot_memmap_count; i++) {
		const struct limine_memmap_entry* entry = memmap_req.response->entries[i];
		if (entry == NULL) return false;

		boot_memmap[i] = (struct mem_range){
			.base   = (uintptr_t)entry->base,
			.length = (size_t)entry->length,
			.type   = kernel_boot_mem_range_type(entry->type),
		};
	}

	boot_address_space = (struct kernel_boot_address_space){
		.direct_map_offset = (uintptr_t)hhdm_req.response->offset,
		.physical_base     = (uintptr_t)exec_addr_req.response->physical_base,
		.virtual_base      = (uintptr_t)exec_addr_req.response->virtual_base,
	};
	boot_address_space_valid = true;

	boot_cmdline = (cmdline_req.response != NULL) ? cmdline_req.response->cmdline : NULL;

	if (fb_req.response != NULL && fb_req.response->framebuffer_count > 0u && fb_req.response->framebuffers != NULL &&
	    fb_req.response->framebuffers[0] != NULL && fb_req.response->framebuffers[0]->address != NULL) {
		const struct limine_framebuffer* fb = fb_req.response->framebuffers[0];

		boot_framebuffer = (struct kernel_boot_framebuffer){
			.address = (void*)(uintptr_t)fb->address,
			.width   = fb->width,
			.height  = fb->height,
			.pitch   = fb->pitch,
			.bpp     = fb->bpp,
		};
		boot_framebuffer_valid = true;
	}

	if (rsdp_req.response != NULL && rsdp_req.response->address != NULL) {
		boot_rsdp_address = (uintptr_t)rsdp_req.response->address;
		boot_rsdp_valid   = true;
	}

	for (size_t i = 0; i < KERNEL_BOOT_MAX_CPUS; i++) {
		boot_cpu_private[i] = NULL;
		boot_cpu_launch[i]  = (struct kernel_boot_cpu_launch){0};
	}
	boot_cpu_count   = 0u;
	boot_initialized = true;
	return true;
}

bool kernel_boot_protocol_supported(void) {
	return boot_initialized;
}

bool kernel_boot_framebuffer_get(struct kernel_boot_framebuffer* out) {
	if (out == NULL || !boot_framebuffer_valid) return false;
	*out = boot_framebuffer;
	return true;
}

const struct mem_range* kernel_boot_memmap(size_t* out_count) {
	if (!boot_initialized) return NULL;
	if (out_count != NULL) *out_count = boot_memmap_count;
	return boot_memmap;
}

const char* kernel_boot_cmdline(void) {
	return boot_cmdline;
}

bool kernel_boot_rsdp_address(uintptr_t* out_address) {
	if (out_address == NULL || !boot_rsdp_valid) return false;
	*out_address = boot_rsdp_address;
	return true;
}

bool kernel_boot_address_space_get(struct kernel_boot_address_space* out) {
	if (out == NULL || !boot_address_space_valid) return false;
	*out = boot_address_space;
	return true;
}

bool kernel_boot_cpu_mp_supported(void) {
	return kernel_boot_mp_supported();
}

bool kernel_boot_cpu_topology(struct cpu_init_info* init_info, size_t max_count, uintptr_t boot_stack_base,
                              uintptr_t boot_stack_top, size_t* out_cpu_count, size_t* out_bsp_index) {
	size_t cpu_count = 1u;
	size_t bsp_index = 0u;

	if (init_info == NULL || out_cpu_count == NULL || out_bsp_index == NULL || max_count == 0u || !boot_initialized)
		return false;

	init_info[0] = (struct cpu_init_info){
		.index           = 0u,
		.processor_id    = 0u,
		.arch_id         = hal_cpu_boot_arch_id(),
		.role            = CPU_ROLE_BSP,
		.boot_stack_base = boot_stack_base,
		.boot_stack_top  = boot_stack_top,
	};

	if (kernel_boot_mp_supported() && mp_req.response != NULL && mp_req.response->cpus != NULL &&
	    mp_req.response->cpu_count > 0u) {
		uint64_t bsp_arch_id = kernel_boot_mp_bsp_arch_id(mp_req.response);

		if (mp_req.response->cpu_count > max_count || mp_req.response->cpu_count > KERNEL_BOOT_MAX_CPUS) return false;
		cpu_count = (size_t)mp_req.response->cpu_count;
		for (size_t i = 0; i < cpu_count; i++) {
			const struct LIMINE_MP(info)* info = mp_req.response->cpus[i];
			uint64_t      arch_id              = kernel_boot_mp_info_arch_id(info);
			enum cpu_role role                 = arch_id == bsp_arch_id ? CPU_ROLE_BSP : CPU_ROLE_AP;
			uintptr_t     stack_base           = role == CPU_ROLE_BSP ? boot_stack_base : 0u;
			uintptr_t     stack_top            = role == CPU_ROLE_BSP ? boot_stack_top : 0u;

			init_info[i] = (struct cpu_init_info){
				.index           = i,
				.processor_id    = kernel_boot_mp_info_processor_id(info),
				.arch_id         = arch_id,
				.role            = role,
				.boot_stack_base = stack_base,
				.boot_stack_top  = stack_top,
			};
			boot_cpu_private[i] = (void*)info;
			if (role == CPU_ROLE_BSP) bsp_index = i;
		}
	}

	boot_cpu_count = cpu_count;
	*out_cpu_count = cpu_count;
	*out_bsp_index = bsp_index;
	return true;
}

bool kernel_boot_cpu_start(size_t cpu_index, kernel_boot_cpu_entry_t entry, void* arg) {
#if defined(PLATFORM_PC_X86_64) || defined(PLATFORM_PC_AARCH64) || defined(PLATFORM_PC_RISCV64)
	struct LIMINE_MP(info) * info;

	if (!boot_initialized || entry == NULL || cpu_index >= boot_cpu_count || cpu_index >= KERNEL_BOOT_MAX_CPUS)
		return false;

	info = (struct LIMINE_MP(info)*)boot_cpu_private[cpu_index];
	if (info == NULL) return false;

	boot_cpu_launch[cpu_index] = (struct kernel_boot_cpu_launch){
		.entry     = entry,
		.arg       = arg,
		.cpu_index = cpu_index,
	};
	__atomic_store_n(&info->extra_argument, (uint64_t)(uintptr_t)&boot_cpu_launch[cpu_index], __ATOMIC_SEQ_CST);
	__atomic_store_n(&info->goto_address, kernel_boot_mp_entry, __ATOMIC_SEQ_CST);
	return true;
#else
	(void)cpu_index;
	(void)entry;
	(void)arg;
	return false;
#endif
}
