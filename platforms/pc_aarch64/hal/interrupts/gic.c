#include "gic.h"

#include <core/cpu.h>
#include <hal/interrupts.h>
#include <hal/paging.h>
#include <kernel/boot.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../../../iommu_acpi.h"
#include "../../../iommu_fdt.h"
#include "../clock.h"
#include "gicv3.h"

#define AARCH64_MMIO_PAGE_SIZE 0x1000u
#define AARCH64_GIC_MAX_CPUS 64u
#define AARCH64_GIC_MAX_TARGETS 8u
#define AARCH64_GIC_SCHEDULER_SGI 1u
#define AARCH64_GIC_TIMER_PPI 27u

#define AARCH64_GICD_CTLR 0x000u
#define AARCH64_GICD_IGROUPR0 0x080u
#define AARCH64_GICD_ISENABLER0 0x100u
#define AARCH64_GICD_ICENABLER0 0x180u
#define AARCH64_GICD_TYPER 0x004u
#define AARCH64_GICD_IPRIORITYR 0x400u
#define AARCH64_GICD_ITARGETSR 0x800u
#define AARCH64_GICD_ICFGR 0xc00u

#define AARCH64_GICC_CTLR 0x000u
#define AARCH64_GICC_PMR 0x004u
#define AARCH64_GICC_IAR 0x00cu
#define AARCH64_GICC_EOIR 0x010u

#define AARCH64_GICD_CTLR_ENABLE (1u << 0)
#define AARCH64_GICC_CTLR_ENABLE (1u << 0)
#define AARCH64_GICC_IAR_INTID_MASK 0x3ffu
#define AARCH64_GICC_INTID_SPURIOUS_MIN 1020u

static bool              gic_ready;
static volatile uint8_t* gicd_mmio;
static volatile uint8_t* gicc_mmio;
static uint8_t           gic_target_masks[AARCH64_GIC_MAX_CPUS];

static inline uintptr_t phys_to_virt(uintptr_t phys) {
	struct kernel_boot_address_space address_space;

	if (!kernel_boot_address_space_get(&address_space)) return 0u;
	return address_space.direct_map_offset + phys;
}

static inline uint8_t mmio_read8(volatile uint8_t* base, uint32_t offset) {
	return *(volatile uint8_t*)(base + offset);
}

static inline uint32_t mmio_read32(volatile uint8_t* base, uint32_t offset) {
	return *(volatile uint32_t*)(base + offset);
}

static inline void mmio_write32(volatile uint8_t* base, uint32_t offset, uint32_t value) {
	*(volatile uint32_t*)(base + offset) = value;
}

static inline void mmio_write8(volatile uint8_t* base, uint32_t offset, uint8_t value) {
	*(volatile uint8_t*)(base + offset) = value;
}

static inline void sync(void) {
	__asm__ volatile("dsb sy\n\t"
	                 "isb" ::
	                     : "memory");
}

static bool map_mmio_page(uintptr_t phys) {
	uintptr_t page_phys = phys & ~(uintptr_t)(AARCH64_MMIO_PAGE_SIZE - 1u);
	uintptr_t page_virt;
	page_virt = phys_to_virt(page_phys);
	if (page_virt == 0u) return false;
	if (hal_paging_query(hal_paging_kernel_space(), page_virt, NULL)) return true;

	return hal_paging_map(hal_paging_kernel_space(),
	                      &(const struct hal_paging_map_request){page_virt,
	                                                             page_phys,
	                                                             AARCH64_MMIO_PAGE_SIZE,
	                                                             HAL_PAGE_READ | HAL_PAGE_WRITE | HAL_PAGE_GLOBAL,
	                                                             MEMORY_TYPE_DEVICE});
}

static bool gic_is_ready(void) {
	return __atomic_load_n(&gic_ready, __ATOMIC_ACQUIRE);
}

static bool gic_find_acpi_v2(uintptr_t* out_distributor, uintptr_t* out_cpu_interface) {
	const struct iommu_acpi_header* madt          = iommu_acpi_table("APIC");
	uintptr_t                       distributor   = 0u;
	uintptr_t                       cpu_interface = 0u;
	uint8_t                         version       = 0u;
	if (madt == NULL || madt->length < sizeof(*madt) + 8u) return false;
	const uint8_t* entry = (const uint8_t*)madt + sizeof(*madt) + 8u;
	const uint8_t* end   = (const uint8_t*)madt + madt->length;
	while ((size_t)(end - entry) >= 2u) {
		uint8_t type   = entry[0];
		uint8_t length = entry[1];
		if (length < 2u || length > (size_t)(end - entry)) return false;
		if (type == 0xcu && length >= 24u) {
			uint64_t address;
			memcpy(&address, entry + 8u, sizeof(address));
			if (distributor != 0u || address == 0u || address > UINTPTR_MAX) return false;
			distributor = (uintptr_t)address;
			version     = entry[20u];
		}
		if (type == 0xbu && length >= 40u) {
			uint32_t flags;
			uint64_t address;
			memcpy(&flags, entry + 12u, sizeof(flags));
			memcpy(&address, entry + 32u, sizeof(address));
			if ((flags & 1u) != 0u && cpu_interface == 0u && address != 0u && address <= UINTPTR_MAX)
				cpu_interface = (uintptr_t)address;
		}
		entry += length;
	}
	if (entry != end || distributor == 0u || cpu_interface == 0u || (version != 1u && version != 2u)) return false;
	*out_distributor   = distributor;
	*out_cpu_interface = cpu_interface;
	return true;
}

static bool gic_find_fdt_v2(uintptr_t* out_distributor, uintptr_t* out_cpu_interface) {
	static const char* const compatibles[] = {
		"arm,gic-400", "arm,cortex-a15-gic", "arm,cortex-a7-gic", "arm,gic-v2", "arm,pl390"};
	for (size_t i = 0u; i < sizeof(compatibles) / sizeof(compatibles[0]); i++) {
		uintptr_t distributor   = 0u;
		uintptr_t cpu_interface = 0u;
		if (iommu_fdt_controllers_full(compatibles[i], 0u, &distributor, &cpu_interface) == 0u) continue;
		if (distributor == 0u || cpu_interface == 0u) return false;
		*out_distributor   = distributor;
		*out_cpu_interface = cpu_interface;
		return true;
	}
	return false;
}

static bool gic_init_global(void) {
	uintptr_t distributor;
	uintptr_t cpu_interface;
	if (aarch64_gicv3_described()) return aarch64_gicv3_init_global();
	if (gic_is_ready()) return true;
	if (!gic_find_fdt_v2(&distributor, &cpu_interface) && !gic_find_acpi_v2(&distributor, &cpu_interface)) return false;

	if (!map_mmio_page(distributor) || !map_mmio_page(cpu_interface)) {
		return false;
	}

	gicd_mmio = (volatile uint8_t*)(uintptr_t)phys_to_virt(distributor);
	gicc_mmio = (volatile uint8_t*)(uintptr_t)phys_to_virt(cpu_interface);

	mmio_write32(gicd_mmio, AARCH64_GICD_CTLR, 0u);
	mmio_write32(gicd_mmio, AARCH64_GICD_CTLR, AARCH64_GICD_CTLR_ENABLE);
	sync();

	__atomic_store_n(&gic_ready, true, __ATOMIC_RELEASE);
	return true;
}

static bool gic_target_mask_valid(uint8_t mask) {
	return mask != 0u && (mask & (uint8_t)(mask - 1u)) == 0u;
}

bool aarch64_gic_init_local(struct cpu* cpu) {
	uint32_t group;
	uint8_t  target_mask;
	if (aarch64_gicv3_ready()) return aarch64_gicv3_init_local(cpu);

	if (cpu == NULL || cpu != cpu_current() || cpu->index >= AARCH64_GIC_MAX_CPUS) return false;
	if (!gic_is_ready()) return true;

	group = mmio_read32(gicd_mmio, AARCH64_GICD_IGROUPR0);
	group &= ~(1u << AARCH64_GIC_SCHEDULER_SGI);
	mmio_write32(gicd_mmio, AARCH64_GICD_IGROUPR0, group);
	mmio_write8(gicd_mmio, AARCH64_GICD_IPRIORITYR + AARCH64_GIC_SCHEDULER_SGI, 0x40u);
	mmio_write32(gicd_mmio, AARCH64_GICD_ISENABLER0, 1u << AARCH64_GIC_SCHEDULER_SGI);
	mmio_write32(gicc_mmio, AARCH64_GICC_PMR, 0xffu);
	mmio_write32(gicc_mmio, AARCH64_GICC_CTLR, AARCH64_GICC_CTLR_ENABLE);
	sync();

	/* ITARGETSR0-7 are banked and read-only for SGIs/PPIs on GICv2.
	 * Reading one SGI byte therefore gives this CPU interface's target bit. */
	target_mask = mmio_read8(gicd_mmio, AARCH64_GICD_ITARGETSR + AARCH64_GIC_SCHEDULER_SGI);
	if (!gic_target_mask_valid(target_mask)) target_mask = 0u;
	__atomic_store_n(&gic_target_masks[cpu->index], target_mask, __ATOMIC_RELEASE);
	return true;
}

static bool gic_fixed_interrupt_valid(uint32_t id) {
	uint32_t interrupt_count;

	if (!gic_is_ready() || id < 16u || id >= AARCH64_GICC_INTID_SPURIOUS_MIN) return false;
	interrupt_count = 32u * ((mmio_read32(gicd_mmio, AARCH64_GICD_TYPER) & 0x1fu) + 1u);
	if (interrupt_count > AARCH64_GICC_INTID_SPURIOUS_MIN) interrupt_count = AARCH64_GICC_INTID_SPURIOUS_MIN;
	return id < interrupt_count;
}

static bool gic_set_fixed_enabled(uint32_t id, bool enabled) {
	uint32_t bank;
	uint32_t bit;

	if (!gic_fixed_interrupt_valid(id)) return false;
	bank = (uint32_t)id / 32u;
	bit  = 1u << ((uint32_t)id % 32u);
	mmio_write32(gicd_mmio, (enabled ? AARCH64_GICD_ISENABLER0 : AARCH64_GICD_ICENABLER0) + bank * 4u, bit);
	sync();
	return true;
}

size_t hal_interrupt_source_domain_count(void) {
	if (aarch64_gicv3_described()) return aarch64_gicv3_source_domain_count();
	struct hal_interrupt_source_domain_info domain;
	return hal_interrupt_source_domain_at(0u, &domain) ? 1u : 0u;
}

bool hal_interrupt_source_domain_at(size_t index, struct hal_interrupt_source_domain_info* out_domain) {
	if (aarch64_gicv3_described()) return aarch64_gicv3_source_domain_at(index, out_domain);
	if (index != 0u || out_domain == NULL || !gic_is_ready()) return false;
	uint32_t count = 32u * ((mmio_read32(gicd_mmio, AARCH64_GICD_TYPER) & 0x1fu) + 1u);
	if (count > AARCH64_GICC_INTID_SPURIOUS_MIN) count = AARCH64_GICC_INTID_SPURIOUS_MIN;
	if (count <= 32u) return false;
	*out_domain =
		(struct hal_interrupt_source_domain_info){.domain = 0u, .first_source = 32u, .source_count = count - 32u};
	return true;
}

bool hal_interrupt_source_info(const struct hal_interrupt_source* source, struct hal_interrupt_source_info* out_info) {
	if (aarch64_gicv3_described()) return aarch64_gicv3_source_info(source, out_info);
	if (source == NULL || out_info == NULL || source->domain != 0u || source->number < 32u ||
	    !gic_fixed_interrupt_valid(source->number))
		return false;
	*out_info = (struct hal_interrupt_source_info){
		.delivery     = {.domain = 0u, .base = source->number, .limit = source->number + 1u},
		.target_kind  = HAL_INTERRUPT_TARGET_ROUTABLE,
		.fixed_target = NULL
    };
	return true;
}

bool hal_interrupt_source_target_supported(const struct hal_interrupt_source* source, const struct cpu* target) {
	if (aarch64_gicv3_described()) return aarch64_gicv3_source_target_supported(source, target);
	struct hal_interrupt_source_info info;
	return target != NULL && target->index < AARCH64_GIC_MAX_CPUS && hal_interrupt_source_info(source, &info) &&
	       gic_target_mask_valid(__atomic_load_n(&gic_target_masks[target->index], __ATOMIC_ACQUIRE));
}

static bool gicv2_source_init(struct hal_interrupt_source_state* state, const struct hal_interrupt_source* source,
                              const struct hal_interrupt_delivery* delivery, bool local) {
	uint8_t                          target_mask;
	uint32_t                         bank;
	uint32_t                         bit;
	uint32_t                         group;
	struct hal_interrupt_source_info info;

	if (state == NULL || state->initialized || source == NULL || delivery == NULL || delivery->target == NULL ||
	    delivery->target->index >= AARCH64_GIC_MAX_CPUS || !gic_init_global() ||
	    delivery->trigger > HAL_INTERRUPT_TRIGGER_LEVEL || delivery->polarity > HAL_INTERRUPT_POLARITY_HIGH)
		return false;
	if (local) {
		if (source->domain != 0u || source->number < 16u || source->number >= 32u ||
		    !gic_fixed_interrupt_valid(source->number) || delivery->event.domain != 0u ||
		    delivery->event.id != source->number || delivery->target != cpu_current() ||
		    delivery->trigger != HAL_INTERRUPT_TRIGGER_FIRMWARE)
			return false;
	}
	else if (!hal_interrupt_source_info(source, &info) || delivery->event.domain != info.delivery.domain ||
	         delivery->event.id != info.delivery.base ||
	         !hal_interrupt_source_target_supported(source, delivery->target))
		return false;
	if (delivery->target == cpu_current() && !aarch64_gic_init_local(cpu_current())) return false;
	target_mask = 0u;
	if (source->number >= 32u) {
		target_mask = __atomic_load_n(&gic_target_masks[delivery->target->index], __ATOMIC_ACQUIRE);
		if (!gic_target_mask_valid(target_mask)) return false;
	}
	if (!gic_set_fixed_enabled(source->number, false)) return false;
	bank  = source->number / 32u;
	bit   = 1u << (source->number % 32u);
	group = mmio_read32(gicd_mmio, AARCH64_GICD_IGROUPR0 + bank * 4u);
	group &= ~bit;
	mmio_write32(gicd_mmio, AARCH64_GICD_IGROUPR0 + bank * 4u, group);
	mmio_write8(gicd_mmio, AARCH64_GICD_IPRIORITYR + source->number, 0x80u);
	if (source->number >= 32u) mmio_write8(gicd_mmio, AARCH64_GICD_ITARGETSR + source->number, target_mask);
	if (delivery->trigger != HAL_INTERRUPT_TRIGGER_FIRMWARE) {
		uint32_t config_offset = AARCH64_GICD_ICFGR + (source->number / 16u) * 4u;
		uint32_t trigger_bit   = 1u << (((source->number % 16u) * 2u) + 1u);
		uint32_t config        = mmio_read32(gicd_mmio, config_offset);
		if (delivery->trigger == HAL_INTERRUPT_TRIGGER_EDGE) config |= trigger_bit;
		else if (delivery->trigger == HAL_INTERRUPT_TRIGGER_LEVEL) config &= ~trigger_bit;
		else return false;
		mmio_write32(gicd_mmio, config_offset, config);
	}
	sync();
	*state = (struct hal_interrupt_source_state){
		.source = *source, .target_index = delivery->target->index, .initialized = true, .masked = true};
	return true;
}

bool hal_interrupt_source_init(struct hal_interrupt_source_state* state, const struct hal_interrupt_source* source,
                               const struct hal_interrupt_delivery* delivery) {
	if (aarch64_gicv3_described()) return aarch64_gicv3_source_init(state, source, delivery);
	return gicv2_source_init(state, source, delivery, false);
}

bool aarch64_gic_local_source_init(struct hal_interrupt_source_state* state, uint32_t id, const struct cpu* target) {
	if (aarch64_gicv3_described()) return aarch64_gicv3_local_source_init(state, id, target);
	struct hal_interrupt_source   source   = {.domain = 0u, .number = id};
	struct hal_interrupt_delivery delivery = {
		.target   = target,
		.event    = {.domain = 0u, .id = id},
		.trigger  = HAL_INTERRUPT_TRIGGER_FIRMWARE,
		.polarity = HAL_INTERRUPT_POLARITY_FIRMWARE
    };
	return gicv2_source_init(state, &source, &delivery, true);
}

bool hal_interrupt_source_mask(struct hal_interrupt_source_state* state) {
	if (aarch64_gicv3_ready()) return aarch64_gicv3_source_mask(state);
	if (state == NULL || !state->initialized ||
	    (state->source.number < 32u && (cpu_current() == NULL || cpu_current()->index != state->target_index)) ||
	    !gic_set_fixed_enabled(state->source.number, false))
		return false;
	state->masked = true;
	return true;
}

bool hal_interrupt_source_unmask(struct hal_interrupt_source_state* state) {
	if (aarch64_gicv3_ready()) return aarch64_gicv3_source_unmask(state);
	if (state == NULL || !state->initialized ||
	    (state->source.number < 32u && (cpu_current() == NULL || cpu_current()->index != state->target_index)) ||
	    !gic_set_fixed_enabled(state->source.number, true))
		return false;
	state->masked = false;
	return true;
}

bool hal_interrupt_source_deinit(struct hal_interrupt_source_state* state) {
	if (aarch64_gicv3_ready()) {
		return aarch64_gicv3_source_deinit(state);
	}
	if (state == NULL) return false;
	if (!state->initialized) return true;
	if (!hal_interrupt_source_mask(state)) return false;
	state->initialized = false;
	return true;
}

bool aarch64_gic_local_source_unmask(struct hal_interrupt_source_state* state) {
	return hal_interrupt_source_unmask(state);
}

bool aarch64_gic_local_source_deinit(struct hal_interrupt_source_state* state) {
	return hal_interrupt_source_deinit(state);
}

static bool gic_v2m_frame_phys(uintptr_t* out_frame) {
	uintptr_t frame = 0u;
	if (out_frame == NULL) return false;
	if (iommu_fdt_controllers("arm,gic-v2m-frame", 0u, &frame) != 0u && frame != 0u) {
		*out_frame = frame;
		return true;
	}
	const struct iommu_acpi_header* madt = iommu_acpi_table("APIC");
	if (madt == NULL || madt->length < sizeof(*madt) + 8u) return false;
	const uint8_t* entry = (const uint8_t*)madt + sizeof(*madt) + 8u;
	const uint8_t* end   = (const uint8_t*)madt + madt->length;
	while ((size_t)(end - entry) >= 2u) {
		uint8_t length = entry[1];
		if (length < 2u || length > (size_t)(end - entry)) return false;
		if (entry[0] == 0xdu && length >= 24u) {
			uint64_t address;
			memcpy(&address, entry + 8u, sizeof(address));
			if (address == 0u || address > UINTPTR_MAX) return false;
			*out_frame = (uintptr_t)address;
			return true;
		}
		entry += length;
	}
	return false;
}

static bool gic_v2m_range(uintptr_t* out_frame, uint32_t* out_base, uint32_t* out_count) {
	uintptr_t frame;
	if (!gic_is_ready() || !gic_v2m_frame_phys(&frame) || !map_mmio_page(frame)) return false;
	uint32_t typer = mmio_read32((volatile uint8_t*)phys_to_virt(frame), 0x8u);
	uint32_t first = (typer >> 16u) & 0x3ffu;
	uint32_t count = typer & 0x3ffu;
	if (first < 32u || first >= AARCH64_GICC_INTID_SPURIOUS_MIN || count == 0u ||
	    count > AARCH64_GICC_INTID_SPURIOUS_MIN - first || !gic_fixed_interrupt_valid(first + count - 1u))
		return false;
	*out_frame = frame;
	*out_base  = first;
	*out_count = count;
	return true;
}

size_t hal_interrupt_message_range_count(void) {
	if (aarch64_gicv3_described()) return aarch64_gicv3_message_range_count();
	uintptr_t frame;
	uint32_t  first;
	uint32_t  count;
	return gic_v2m_range(&frame, &first, &count) ? 1u : 0u;
}

bool hal_interrupt_message_range_at(size_t index, struct hal_interrupt_message_range* out_range) {
	if (aarch64_gicv3_described()) return aarch64_gicv3_message_range_at(index, out_range);
	uintptr_t frame;
	uint32_t  first;
	uint32_t  count;
	if (index != 0u || out_range == NULL || !gic_v2m_range(&frame, &first, &count)) return false;
	*out_range = (struct hal_interrupt_message_range){
		.domain = 0u, .delivery = {.domain = 0u, .base = first, .limit = first + count}
    };
	return true;
}

bool hal_interrupt_message_target_supported(uint32_t domain, const struct hal_interrupt_message_source* source,
                                            const struct cpu* target) {
	if (aarch64_gicv3_described()) return aarch64_gicv3_message_target_supported(domain, source, target);
	uintptr_t frame;
	uint32_t  first;
	uint32_t  count;
	return domain == 0u && source == NULL && target != NULL && target->index < AARCH64_GIC_MAX_CPUS &&
	       gic_v2m_range(&frame, &first, &count) &&
	       gic_target_mask_valid(__atomic_load_n(&gic_target_masks[target->index], __ATOMIC_ACQUIRE));
}

bool hal_interrupt_message_init(struct hal_interrupt_message_state*         state,
                                const struct hal_interrupt_message_request* request,
                                struct hal_interrupt_message*               out_message) {
	if (aarch64_gicv3_described()) return aarch64_gicv3_message_init(state, request, out_message);
	uintptr_t frame;
	uint32_t  spi_base;
	uint32_t  spi_count;
	uint8_t   target_mask;
	if (state == NULL || state->initialized || request == NULL || out_message == NULL ||
	    !hal_interrupt_message_target_supported(request->domain, request->source, request->target) ||
	    request->event.domain != 0u || !gic_is_ready() || !gic_v2m_range(&frame, &spi_base, &spi_count))
		return false;
	target_mask = __atomic_load_n(&gic_target_masks[request->target->index], __ATOMIC_ACQUIRE);
	if (!gic_target_mask_valid(target_mask)) return false;
	if (request->event.id < spi_base || request->event.id - spi_base >= spi_count) return false;
	uint32_t id = request->event.id;
	if (!gic_set_fixed_enabled(id, false)) return false;
	uint32_t bank  = id / 32u;
	uint32_t group = mmio_read32(gicd_mmio, AARCH64_GICD_IGROUPR0 + bank * 4u);
	group &= ~(1u << (id % 32u));
	mmio_write32(gicd_mmio, AARCH64_GICD_IGROUPR0 + bank * 4u, group);
	mmio_write8(gicd_mmio, AARCH64_GICD_IPRIORITYR + id, 0x80u);
	mmio_write8(gicd_mmio, AARCH64_GICD_ITARGETSR + id, target_mask);
	if (!gic_set_fixed_enabled(id, true)) return false;
	*out_message = (struct hal_interrupt_message){.address = (uint64_t)frame + 0x40u, .data = id};
	*state = (struct hal_interrupt_message_state){.event = request->event, .uses_gicv3 = false, .initialized = true};
	return true;
}

bool hal_interrupt_message_deinit(struct hal_interrupt_message_state* state) {
	if (state == NULL) return false;
	if (!state->initialized) return true;
	if (state->uses_gicv3) return aarch64_gicv3_message_deinit(state);
	if (state->event.domain != 0u || !gic_set_fixed_enabled(state->event.id, false)) return false;
	state->initialized = false;
	return true;
}

bool aarch64_gic_prepare_smp(void) {
	if (aarch64_gicv3_described()) return aarch64_gicv3_prepare_smp();
	if (cpu_count() > AARCH64_GIC_MAX_TARGETS) return false;
	if (!gic_init_global()) return false;
	if (!aarch64_gic_init_local(cpu_current())) return false;
	return cpu_count() == 1u || gic_target_mask_valid(gic_target_masks[cpu_current()->index]);
}

static bool is_irq_vector(uint64_t vector) {
	switch (vector) {
	case 1u:
	case 5u:
	case 9u:
	case 13u:
		return true;
	default:
		return false;
	}
}

static bool gic_handle_external_irq(const struct exception_frame* frame) {
	uint32_t iar;
	uint32_t intid;
	bool     handled;

	if (frame == NULL || !is_irq_vector(frame->vector) || !gic_is_ready()) return false;
	iar   = mmio_read32(gicc_mmio, AARCH64_GICC_IAR);
	intid = iar & AARCH64_GICC_IAR_INTID_MASK;
	if (intid >= AARCH64_GICC_INTID_SPURIOUS_MIN) return true;
	if (intid == AARCH64_GIC_SCHEDULER_SGI) {
		handled = true;
	}
	else if (intid == AARCH64_GIC_TIMER_PPI) {
		(void)aarch64_clock_fire();
		handled = true;
	}
	else {
		if (intid < 32u) mmio_write32(gicd_mmio, AARCH64_GICD_ICENABLER0, 1u << intid);
		else (void)gic_set_fixed_enabled(intid, false);
		handled = true;
	}
	mmio_write32(gicc_mmio, AARCH64_GICC_EOIR, iar);
	return handled;
}

bool aarch64_gic_handle_irq(const struct exception_frame* frame) {
	if (aarch64_gicv3_ready()) return aarch64_gicv3_handle_irq(frame);
	return gic_handle_external_irq(frame);
}
