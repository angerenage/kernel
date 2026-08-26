#include <base/math.h>
#include <hal/cpu.h>
#include <kernel/boot.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "limine_requests.h"

#define KERNEL_BOOT_MAX_CPUS 64u
#define KERNEL_BOOT_MAX_MEM_RANGES 256u
#define KERNEL_BOOT_MAX_MODULES 64u

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
static struct kernel_boot_module        boot_modules[KERNEL_BOOT_MAX_MODULES];
static size_t                           boot_module_count;
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

static const char* kernel_boot_path_basename(const char* path) {
	const char* basename = path;

	if (path == NULL) return NULL;
	for (const char* cursor = path; *cursor != '\0'; cursor++) {
		if (*cursor == '/' || *cursor == '\\') basename = cursor + 1;
	}
	return basename;
}

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
	for (size_t i = 0u; i < (size_t)memmap_req.response->entry_count; i++) {
		if (memmap_req.response->entries[i] == NULL) return false;
	}
	if (module_req.response != NULL && module_req.response->module_count > 0u) {
		if (module_req.response->module_count > KERNEL_BOOT_MAX_MODULES || module_req.response->modules == NULL) {
			return false;
		}
		for (size_t i = 0u; i < (size_t)module_req.response->module_count; i++) {
			const struct limine_file* file = module_req.response->modules[i];
			if (file == NULL || file->address == NULL) return false;
		}
	}

	boot_memmap_count = (size_t)memmap_req.response->entry_count;
	for (size_t i = 0; i < boot_memmap_count; i++) {
		const struct limine_memmap_entry* entry = memmap_req.response->entries[i];

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

	boot_cmdline = (cmdline_req.response != NULL) ? cmdline_req.response->cmdline : NULL;

	boot_module_count = 0u;
	if (module_req.response != NULL && module_req.response->module_count > 0u) {
		boot_module_count = (size_t)module_req.response->module_count;
		for (size_t i = 0; i < boot_module_count; i++) {
			const struct limine_file* file = module_req.response->modules[i];

			boot_modules[i] = (struct kernel_boot_module){
				.path = file->path,
#if LIMINE_API_REVISION >= 3
				.name = file->string,
#else
				.name = file->cmdline,
#endif
				.address    = (void*)(uintptr_t)file->address,
				.size       = (size_t)file->size,
				.media_type = file->media_type,
			};
		}
	}

	boot_framebuffer_valid = false;
	if (fb_req.response != NULL && fb_req.response->framebuffer_count > 0u && fb_req.response->framebuffers != NULL &&
	    fb_req.response->framebuffers[0] != NULL && fb_req.response->framebuffers[0]->address != NULL) {
		const struct limine_framebuffer* fb = fb_req.response->framebuffers[0];
		size_t                           framebuffer_size;

		if (fb->width != 0u && fb->height != 0u && fb->pitch != 0u && fb->bpp != 0u &&
		    (uint64_t)(size_t)fb->pitch == fb->pitch && (uint64_t)(size_t)fb->height == fb->height &&
		    !mul_overflow_size((size_t)fb->pitch, (size_t)fb->height, &framebuffer_size) && framebuffer_size != 0u) {
			boot_framebuffer = (struct kernel_boot_framebuffer){
				.address          = (void*)(uintptr_t)fb->address,
				.width            = fb->width,
				.height           = fb->height,
				.pitch            = fb->pitch,
				.bpp              = fb->bpp,
				.memory_model     = fb->memory_model,
				.red_mask_size    = fb->red_mask_size,
				.red_mask_shift   = fb->red_mask_shift,
				.green_mask_size  = fb->green_mask_size,
				.green_mask_shift = fb->green_mask_shift,
				.blue_mask_size   = fb->blue_mask_size,
				.blue_mask_shift  = fb->blue_mask_shift,
			};
			boot_framebuffer_valid = true;
		}
	}

	boot_rsdp_valid = false;
	if (rsdp_req.response != NULL && rsdp_req.response->address != NULL) {
		boot_rsdp_address = (uintptr_t)rsdp_req.response->address;
		boot_rsdp_valid   = true;
	}

	for (size_t i = 0; i < KERNEL_BOOT_MAX_CPUS; i++) {
		boot_cpu_private[i] = NULL;
		boot_cpu_launch[i]  = (struct kernel_boot_cpu_launch){0};
	}
	boot_cpu_count           = 0u;
	boot_address_space_valid = true;
	boot_initialized         = true;
	return true;
}

bool kernel_boot_protocol_supported(void) {
	return boot_initialized;
}

bool kernel_boot_framebuffer_get(struct kernel_boot_framebuffer* out) {
	if (out == NULL || !boot_initialized || !boot_framebuffer_valid) return false;
	*out = boot_framebuffer;
	return true;
}

const struct mem_range* kernel_boot_memmap(size_t* out_count) {
	if (!boot_initialized) return NULL;
	if (out_count != NULL) *out_count = boot_memmap_count;
	return boot_memmap;
}

const char* kernel_boot_cmdline(void) {
	return boot_initialized ? boot_cmdline : NULL;
}

size_t kernel_boot_module_count(void) {
	if (!boot_initialized) return 0u;
	return boot_module_count;
}

const struct kernel_boot_module* kernel_boot_module_at(size_t index) {
	if (!boot_initialized || index >= boot_module_count) return NULL;
	return &boot_modules[index];
}

const struct kernel_boot_module* kernel_boot_module_find(const char* name) {
	if (!boot_initialized || name == NULL) return NULL;

	for (size_t i = 0; i < boot_module_count; i++) {
		const struct kernel_boot_module* module   = &boot_modules[i];
		const char*                      basename = kernel_boot_path_basename(module->path);

		if (module->name != NULL && strcmp(module->name, name) == 0) return module;
		if (basename != NULL && strcmp(basename, name) == 0) return module;
	}

	return NULL;
}

bool kernel_boot_rsdp_address(uintptr_t* out_address) {
	if (out_address == NULL || !boot_initialized || !boot_rsdp_valid) return false;
	*out_address = boot_rsdp_address;
	return true;
}

bool kernel_boot_address_space_get(struct kernel_boot_address_space* out) {
	if (out == NULL || !boot_initialized || !boot_address_space_valid) return false;
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
	bool   bsp_found = false;

	if (init_info == NULL || out_cpu_count == NULL || out_bsp_index == NULL || max_count == 0u || !boot_initialized)
		return false;
	*out_cpu_count = 0u;
	*out_bsp_index = SIZE_MAX;

	init_info[0] = (struct cpu_init_info){
		.index           = 0u,
		.processor_id    = 0u,
		.arch_id         = hal_cpu_boot_arch_id(),
		.role            = CPU_ROLE_BSP,
		.boot_stack_base = boot_stack_base,
		.boot_stack_top  = boot_stack_top,
	};

	if (kernel_boot_mp_supported() && mp_req.response != NULL) {
		uint64_t bsp_arch_id = kernel_boot_mp_bsp_arch_id(mp_req.response);

		if (mp_req.response->cpus == NULL || mp_req.response->cpu_count == 0u) return false;
		if (mp_req.response->cpu_count > max_count || mp_req.response->cpu_count > KERNEL_BOOT_MAX_CPUS) return false;
		cpu_count = (size_t)mp_req.response->cpu_count;
		for (size_t i = 0u; i < cpu_count; i++) {
			const struct LIMINE_MP(info)* info = mp_req.response->cpus[i];
			if (info == NULL) return false;
			if (kernel_boot_mp_info_arch_id(info) != bsp_arch_id) continue;
			if (bsp_found) return false;
			bsp_found = true;
			bsp_index = i;
		}
		if (!bsp_found) return false;

		for (size_t i = 0; i < cpu_count; i++) {
			const struct LIMINE_MP(info)* info = mp_req.response->cpus[i];
			uint64_t      arch_id              = kernel_boot_mp_info_arch_id(info);
			enum cpu_role role                 = i == bsp_index ? CPU_ROLE_BSP : CPU_ROLE_AP;
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
