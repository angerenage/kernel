#include <base/math.h>
#include <boot/protocol.h>
#include <hal/cpu.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "backend.h"
#include "requests.h"

#define KERNEL_BOOT_MAX_CPUS 64u
#define KERNEL_BOOT_MAX_MEM_RANGES 256u
#define KERNEL_BOOT_MAX_MODULES 64u

struct boot_cpu_launch {
	boot_cpu_entry_t entry;
	void*            arg;
	size_t           cpu_index;
};

static struct mem_range       boot_memmap[KERNEL_BOOT_MAX_MEM_RANGES];
static struct boot_module     boot_modules[KERNEL_BOOT_MAX_MODULES];
static struct boot_info       limine_info;
static struct boot_cpu_launch boot_cpu_launch[KERNEL_BOOT_MAX_CPUS];
static void*                  boot_cpu_private[KERNEL_BOOT_MAX_CPUS];
static size_t                 boot_cpu_count;
static bool                   boot_initialized;

static enum mem_range_type limine_mem_range_type(uint64_t type) {
	switch (type) {
	case LIMINE_MEMMAP_USABLE:
		return MEM_RANGE_USABLE;
	case LIMINE_MEMMAP_RESERVED:
		return MEM_RANGE_RESERVED;
	case LIMINE_MEMMAP_ACPI_RECLAIMABLE:
	case LIMINE_MEMMAP_ACPI_NVS:
		return MEM_RANGE_ACPI;
	case LIMINE_MEMMAP_BAD_MEMORY:
		return MEM_RANGE_BAD_MEMORY;
	case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE:
		return MEM_RANGE_BOOTLOADER_RECLAIMABLE;
	case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES:
		return MEM_RANGE_KERNEL_AND_MODULES;
	case LIMINE_MEMMAP_FRAMEBUFFER:
		return MEM_RANGE_FRAMEBUFFER;
	case LIMINE_MEMMAP_RESERVED_MAPPED:
		return MEM_RANGE_RESERVED;
	default:
		return MEM_RANGE_OTHER;
	}
}

#if defined(PLATFORM_PC_X86_64)
static uint64_t limine_mp_info_arch_id(const struct limine_mp_info* info) {
	return info ? (uint64_t)info->lapic_id : 0u;
}

static uint64_t limine_mp_info_processor_id(const struct limine_mp_info* info) {
	return info ? (uint64_t)info->processor_id : 0u;
}

static uint64_t limine_mp_bsp_arch_id(const struct limine_mp_response* response) {
	return response ? (uint64_t)response->bsp_lapic_id : 0u;
}

static bool limine_mp_supported(void) {
	return true;
}
#elif defined(PLATFORM_PC_AARCH64)
static uint64_t limine_mp_info_arch_id(const struct limine_mp_info* info) {
	return info ? info->mpidr : 0u;
}

static uint64_t limine_mp_info_processor_id(const struct limine_mp_info* info) {
	return info ? (uint64_t)info->processor_id : 0u;
}

static uint64_t limine_mp_bsp_arch_id(const struct limine_mp_response* response) {
	return response ? response->bsp_mpidr : 0u;
}

static bool limine_mp_supported(void) {
	return true;
}
#elif defined(PLATFORM_PC_RISCV64)
static uint64_t limine_mp_info_arch_id(const struct limine_mp_info* info) {
	return info ? info->hartid : 0u;
}

static uint64_t limine_mp_info_processor_id(const struct limine_mp_info* info) {
	return info ? info->processor_id : 0u;
}

static uint64_t limine_mp_bsp_arch_id(const struct limine_mp_response* response) {
	return response ? response->bsp_hartid : 0u;
}

static bool limine_mp_supported(void) {
	return true;
}
#elif defined(PLATFORM_PC_LOONGARCH64)
static uint64_t limine_mp_info_arch_id(const struct limine_mp_info* info) {
	return info ? info->phys_id : 0u;
}

static uint64_t limine_mp_info_processor_id(const struct limine_mp_info* info) {
	return info ? info->processor_id : 0u;
}

static uint64_t limine_mp_bsp_arch_id(const struct limine_mp_response* response) {
	return response ? response->bsp_phys_id : 0u;
}

static bool limine_mp_supported(void) {
	return true;
}
#else
static uint64_t limine_mp_info_arch_id(const struct limine_mp_info* info) {
	(void)info;
	return 0u;
}

static uint64_t limine_mp_info_processor_id(const struct limine_mp_info* info) {
	(void)info;
	return 0u;
}

static uint64_t limine_mp_bsp_arch_id(const struct limine_mp_response* response) {
	(void)response;
	return 0u;
}

static bool limine_mp_supported(void) {
	return false;
}
#endif

#if defined(PLATFORM_PC_X86_64) || defined(PLATFORM_PC_AARCH64) || defined(PLATFORM_PC_RISCV64) ||                     \
	defined(PLATFORM_PC_LOONGARCH64)
static void limine_mp_entry(struct limine_mp_info* info) {
	struct boot_cpu_launch* launch;

	if (info == NULL) {
		for (;;) {
			hal_cpu_park();
		}
	}

	launch = (struct boot_cpu_launch*)(uintptr_t)info->extra_argument;
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

bool limine_boot_init(void) {
	if (boot_initialized) return true;
	if (!limine_protocol_supported()) return false;
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

	limine_info.memory_map_count = (size_t)memmap_req.response->entry_count;
	for (size_t i = 0; i < limine_info.memory_map_count; i++) {
		const struct limine_memmap_entry* entry = memmap_req.response->entries[i];

		boot_memmap[i] = (struct mem_range){
			.base   = (uintptr_t)entry->base,
			.length = (size_t)entry->length,
			.type   = limine_mem_range_type(entry->type),
		};
	}

	limine_info.memory_map           = boot_memmap;
	limine_info.direct_map_offset    = (uintptr_t)hhdm_req.response->offset;
	limine_info.kernel_physical_base = (uintptr_t)exec_addr_req.response->physical_base;
	limine_info.kernel_virtual_base  = (uintptr_t)exec_addr_req.response->virtual_base;
	limine_info.command_line         = (cmdline_req.response != NULL) ? cmdline_req.response->cmdline : NULL;

	limine_info.module_count = 0u;
	if (module_req.response != NULL && module_req.response->module_count > 0u) {
		limine_info.module_count = (size_t)module_req.response->module_count;
		for (size_t i = 0; i < limine_info.module_count; i++) {
			const struct limine_file* file = module_req.response->modules[i];

			boot_modules[i] = (struct boot_module){
				.path       = file->path,
				.name       = file->string,
				.address    = (void*)(uintptr_t)file->address,
				.size       = (size_t)file->size,
				.media_type = file->media_type,
			};
		}
	}

	limine_info.framebuffer_available = false;
	if (fb_req.response != NULL && fb_req.response->framebuffer_count > 0u && fb_req.response->framebuffers != NULL &&
	    fb_req.response->framebuffers[0] != NULL && fb_req.response->framebuffers[0]->address != NULL) {
		const struct limine_framebuffer* fb = fb_req.response->framebuffers[0];
		size_t                           framebuffer_size;

		if (fb->width != 0u && fb->height != 0u && fb->pitch != 0u && fb->bpp != 0u &&
		    (uint64_t)(size_t)fb->pitch == fb->pitch && (uint64_t)(size_t)fb->height == fb->height &&
		    !mul_overflow_size((size_t)fb->pitch, (size_t)fb->height, &framebuffer_size) && framebuffer_size != 0u) {
			limine_info.framebuffer = (struct boot_framebuffer){
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
			limine_info.framebuffer_available = true;
		}
	}

	limine_info.rsdp_address = 0u;
	if (rsdp_req.response != NULL && rsdp_req.response->address != NULL) {
		limine_info.rsdp_address = (uintptr_t)rsdp_req.response->address;
	}

	limine_info.dtb_address = 0u;
	if (dtb_req.response != NULL && dtb_req.response->dtb_ptr != NULL) {
		limine_info.dtb_address = (uintptr_t)dtb_req.response->dtb_ptr;
	}

	for (size_t i = 0; i < KERNEL_BOOT_MAX_CPUS; i++) {
		boot_cpu_private[i] = NULL;
		boot_cpu_launch[i]  = (struct boot_cpu_launch){0};
	}
	boot_cpu_count      = 0u;
	limine_info.modules = boot_modules;
	if (!boot_info_publish(&limine_info)) return false;
	boot_initialized = true;
	return true;
}

bool boot_cpu_mp_supported(void) {
	return limine_mp_supported();
}

bool boot_cpu_topology(struct cpu_init_info* init_info, size_t max_count, uintptr_t boot_stack_base,
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

	if (limine_mp_supported() && mp_req.response != NULL) {
		uint64_t bsp_arch_id = limine_mp_bsp_arch_id(mp_req.response);

		if (mp_req.response->cpus == NULL || mp_req.response->cpu_count == 0u) return false;
		if (mp_req.response->cpu_count > max_count || mp_req.response->cpu_count > KERNEL_BOOT_MAX_CPUS) return false;
		cpu_count = (size_t)mp_req.response->cpu_count;
		for (size_t i = 0u; i < cpu_count; i++) {
			const struct limine_mp_info* info = mp_req.response->cpus[i];
			if (info == NULL) return false;
			if (limine_mp_info_arch_id(info) != bsp_arch_id) continue;
			if (bsp_found) return false;
			bsp_found = true;
			bsp_index = i;
		}
		if (!bsp_found) return false;

		for (size_t i = 0; i < cpu_count; i++) {
			const struct limine_mp_info* info       = mp_req.response->cpus[i];
			uint64_t                     arch_id    = limine_mp_info_arch_id(info);
			enum cpu_role                role       = i == bsp_index ? CPU_ROLE_BSP : CPU_ROLE_AP;
			uintptr_t                    stack_base = role == CPU_ROLE_BSP ? boot_stack_base : 0u;
			uintptr_t                    stack_top  = role == CPU_ROLE_BSP ? boot_stack_top : 0u;

			init_info[i] = (struct cpu_init_info){
				.index           = i,
				.processor_id    = limine_mp_info_processor_id(info),
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

bool boot_cpu_start(size_t cpu_index, boot_cpu_entry_t entry, void* arg) {
#if defined(PLATFORM_PC_X86_64) || defined(PLATFORM_PC_AARCH64) || defined(PLATFORM_PC_RISCV64) ||                     \
	defined(PLATFORM_PC_LOONGARCH64)
	struct limine_mp_info* info;

	if (!boot_initialized || entry == NULL || cpu_index >= boot_cpu_count || cpu_index >= KERNEL_BOOT_MAX_CPUS)
		return false;

	info = (struct limine_mp_info*)boot_cpu_private[cpu_index];
	if (info == NULL) return false;

	boot_cpu_launch[cpu_index] = (struct boot_cpu_launch){
		.entry     = entry,
		.arg       = arg,
		.cpu_index = cpu_index,
	};
	__atomic_store_n(&info->extra_argument, (uint64_t)(uintptr_t)&boot_cpu_launch[cpu_index], __ATOMIC_SEQ_CST);
	__atomic_store_n(&info->goto_address, limine_mp_entry, __ATOMIC_SEQ_CST);
	return true;
#else
	(void)cpu_index;
	(void)entry;
	(void)arg;
	return false;
#endif
}
