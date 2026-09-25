#include "gicv3.h"

#include <core/cpu.h>
#include <core/interrupt.h>
#include <core/lock.h>
#include <core/spinlock.h>
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

#define GICV3_PAGE_SIZE 0x1000u
#define GICV3_REDIST_STRIDE 0x20000u
#define GICV3_MAX_CPUS 64u
#define GICV3_SPURIOUS_INTID 1020u
#define GICV3_SCHEDULER_SGI 1u
#define GICV3_TIMER_PPI 27u

#define GICD_CTLR 0x0000u
#define GICD_TYPER 0x0004u
#define GICD_SETSPI_NSR 0x0040u
#define GICD_IGROUPR 0x0080u
#define GICD_ISENABLER 0x0100u
#define GICD_ICENABLER 0x0180u
#define GICD_IPRIORITYR 0x0400u
#define GICD_ICFGR 0x0c00u
#define GICD_IROUTER 0x6000u
#define GICD_CTLR_RWP (1u << 31)
#define GICD_CTLR_ARE_NS (1u << 4)
#define GICD_CTLR_ENABLE_G1A (1u << 1)
#define GICD_CTLR_ENABLE_G1 (1u << 0)
#define GICD_TYPER_MBIS (1u << 16)

#define GICR_CTLR 0x0000u
#define GICR_TYPER 0x0008u
#define GICR_WAKER 0x0014u
#define GICR_SGI_BASE 0x10000u
#define GICR_CTLR_RWP (1u << 3)
#define GICR_TYPER_LAST (1u << 4)
#define GICR_TYPER_VLPIS (1u << 1)
#define GICR_WAKER_PROCESSOR_SLEEP (1u << 1)
#define GICR_WAKER_CHILDREN_ASLEEP (1u << 2)

static uintptr_t       distributor_phys;
static uintptr_t       redistributors_phys;
static uintptr_t       redistributors_size;
static uintptr_t       redistributor_stride;
static uintptr_t       redistributor_phys[GICV3_MAX_CPUS];
static bool            local_ready[GICV3_MAX_CPUS];
static bool            ready;
static struct spinlock gicv3_distributor_lock =
	SPINLOCK_INIT_CLASS("gicv3_distributor", SPINLOCK_ORDER_INTERRUPT, SPINLOCK_FLAG_IRQSAVE);

bool aarch64_gicv3_ready(void) {
	return __atomic_load_n(&ready, __ATOMIC_ACQUIRE);
}

static uintptr_t gicv3_virt(uintptr_t phys) {
	struct kernel_boot_address_space address_space;
	if (!kernel_boot_address_space_get(&address_space) || phys > UINTPTR_MAX - address_space.direct_map_offset)
		return 0u;
	return address_space.direct_map_offset + phys;
}

static bool gicv3_map(uintptr_t phys) {
	uintptr_t page = phys & ~(uintptr_t)(GICV3_PAGE_SIZE - 1u);
	uintptr_t virt = gicv3_virt(page);
	if (virt == 0u) return false;
	if (hal_paging_query(hal_paging_kernel_space(), virt, NULL)) return true;
	return hal_paging_map(
		hal_paging_kernel_space(),
		&(const struct hal_paging_map_request){
			virt, page, GICV3_PAGE_SIZE, HAL_PAGE_READ | HAL_PAGE_WRITE | HAL_PAGE_GLOBAL, MEMORY_TYPE_DEVICE});
}

static uint32_t gicv3_read32(uintptr_t phys) {
	return *(volatile uint32_t*)gicv3_virt(phys);
}
static uint64_t gicv3_read64(uintptr_t phys) {
	return *(volatile uint64_t*)gicv3_virt(phys);
}
static void gicv3_write32(uintptr_t phys, uint32_t value) {
	*(volatile uint32_t*)gicv3_virt(phys) = value;
}
static void gicv3_write64(uintptr_t phys, uint64_t value) {
	*(volatile uint64_t*)gicv3_virt(phys) = value;
}
static void gicv3_write8(uintptr_t phys, uint8_t value) {
	*(volatile uint8_t*)gicv3_virt(phys) = value;
}

/* Wait for distributor writes to reach the interrupt controller. */
static bool gicv3_wait_rwp(void) {
	for (uint32_t attempt = 0u; attempt < 100000u; attempt++)
		if ((gicv3_read32(distributor_phys + GICD_CTLR) & GICD_CTLR_RWP) == 0u) return true;
	return false;
}

/* Wait until an interrupt disable has reached its distributor or redistributor. */
static bool gicv3_wait_disabled(uintptr_t base, uint32_t id) {
	uintptr_t ctlr;
	uint32_t  rwp;
	if (id < 32u) {
		if (base < GICR_SGI_BASE) return false;
		ctlr = base - GICR_SGI_BASE + GICR_CTLR;
		rwp  = GICR_CTLR_RWP;
	}
	else {
		ctlr = distributor_phys + GICD_CTLR;
		rwp  = GICD_CTLR_RWP;
	}
	if (!gicv3_map(ctlr)) return false;
	for (uint32_t attempt = 0u; attempt < 100000u; attempt++)
		if ((gicv3_read32(ctlr) & rwp) == 0u) return true;
	return false;
}

static bool gicv3_find_acpi(uintptr_t* out_distributor, uintptr_t* out_redistributors, uintptr_t* out_size,
                            bool* out_described) {
	const struct iommu_acpi_header* madt           = iommu_acpi_table("APIC");
	uintptr_t                       distributor    = 0u;
	uintptr_t                       redistributors = 0u;
	uintptr_t                       size           = 0u;
	bool                            described      = false;
	if (madt == NULL || madt->length < sizeof(*madt) + 8u) return false;
	const uint8_t* entry = (const uint8_t*)madt + sizeof(*madt) + 8u;
	const uint8_t* end   = (const uint8_t*)madt + madt->length;
	while ((size_t)(end - entry) >= 2u) {
		uint8_t length = entry[1];
		if (length < 2u || length > (size_t)(end - entry)) return false;
		if (entry[0] == 0xcu && length >= 24u && entry[20u] >= 3u) {
			uint64_t address;
			described = true;
			memcpy(&address, entry + 8u, sizeof(address));
			if (distributor != 0u || address == 0u || address > UINTPTR_MAX) return false;
			distributor = (uintptr_t)address;
		}
		if (entry[0] == 0xeu && length >= 16u) {
			uint64_t address;
			uint32_t region_size;
			memcpy(&address, entry + 4u, sizeof(address));
			memcpy(&region_size, entry + 12u, sizeof(region_size));
			if (redistributors != 0u || address == 0u || address > UINTPTR_MAX) return false;
			redistributors = (uintptr_t)address;
			size           = region_size;
		}
		entry += length;
	}
	if (out_described != NULL) *out_described = described;
	if (entry != end || !described || distributor == 0u || redistributors == 0u || size < GICV3_REDIST_STRIDE ||
	    redistributors > UINTPTR_MAX - size)
		return false;
	*out_distributor    = distributor;
	*out_redistributors = redistributors;
	*out_size           = size;
	return true;
}

bool aarch64_gicv3_described(void) {
	uintptr_t distributor;
	uintptr_t redistributors;
	uintptr_t size;
	bool      described = false;
	if (iommu_fdt_compatible_present("arm,gic-v3")) return true;
	(void)gicv3_find_acpi(&distributor, &redistributors, &size, &described);
	return described;
}

bool aarch64_gicv3_init_global(void) {
	uintptr_t distributor    = 0u;
	uintptr_t redistributors = 0u;
	uintptr_t size           = 0u;
	uintptr_t stride         = 0u;
	if (__atomic_load_n(&ready, __ATOMIC_ACQUIRE)) return true;
	if (iommu_fdt_compatible_present("arm,gic-v3")) {
		if (iommu_fdt_controllers_second_region("arm,gic-v3", 0u, &distributor, &redistributors, &size) != 1u)
			return false;
		const uint8_t* property;
		size_t         property_size;
		if (iommu_fdt_controller_property("arm,gic-v3", 0u, "#redistributor-regions", &property, &property_size) &&
		    (property_size != 4u || iommu_fdt_u32(property) != 1u))
			return false;
		if (iommu_fdt_controller_property("arm,gic-v3", 0u, "redistributor-stride", &property, &property_size)) {
			uint64_t value;
			if (property_size != 8u || !iommu_fdt_cells(property, 2u, &value) || value > UINTPTR_MAX ||
			    value < GICV3_REDIST_STRIDE || (value & 0xffffu) != 0u)
				return false;
			stride = (uintptr_t)value;
		}
	}
	else if (!gicv3_find_acpi(&distributor, &redistributors, &size, NULL)) return false;
	if (distributor == 0u || distributor > UINTPTR_MAX - 0x8000u || redistributors == 0u ||
	    size < GICV3_REDIST_STRIDE || redistributors > UINTPTR_MAX - size || !gicv3_map(distributor))
		return false;
	distributor_phys     = distributor;
	redistributors_phys  = redistributors;
	redistributors_size  = size;
	redistributor_stride = stride;

	/* Disable firmware groups without changing an already-enabled ARE_NS. */
	uint32_t ctlr = gicv3_read32(distributor_phys + GICD_CTLR) & GICD_CTLR_ARE_NS;
	gicv3_write32(distributor_phys + GICD_CTLR, ctlr);
	if (!gicv3_wait_rwp()) return false;
	/* Affinity routing must be configured before Group 1 is enabled. */
	ctlr |= GICD_CTLR_ARE_NS;
	gicv3_write32(distributor_phys + GICD_CTLR, ctlr);
	if (!gicv3_wait_rwp()) return false;
	uint32_t interrupt_count = 32u * ((gicv3_read32(distributor_phys + GICD_TYPER) & 0x1fu) + 1u);
	if (interrupt_count > GICV3_SPURIOUS_INTID) interrupt_count = GICV3_SPURIOUS_INTID;
	for (uint32_t bank = 1u; bank < (interrupt_count + 31u) / 32u; bank++)
		gicv3_write32(distributor_phys + GICD_ICENABLER + bank * 4u, UINT32_MAX);
	if (!gicv3_wait_rwp()) return false;
	gicv3_write32(distributor_phys + GICD_CTLR, ctlr | GICD_CTLR_ENABLE_G1A | GICD_CTLR_ENABLE_G1);
	if (!gicv3_wait_rwp()) return false;
	__atomic_store_n(&ready, true, __ATOMIC_RELEASE);
	return true;
}

static bool gicv3_redist_for_cpu(const struct cpu* cpu, uintptr_t* out) {
	if (cpu == NULL || out == NULL || cpu->index >= GICV3_MAX_CPUS || !ready) return false;
	if (redistributor_phys[cpu->index] != 0u) {
		*out = redistributor_phys[cpu->index];
		return true;
	}
	uint32_t affinity = (uint32_t)(((cpu->arch_id >> 32u) & 0xffu) << 24u) | (uint32_t)(cpu->arch_id & 0x00ffffffu);
	for (uintptr_t offset = 0u; offset <= redistributors_size - GICV3_REDIST_STRIDE;) {
		uintptr_t frame = redistributors_phys + offset;
		if (!gicv3_map(frame)) return false;
		uint64_t typer = gicv3_read64(frame + GICR_TYPER);
		if ((uint32_t)(typer >> 32u) == affinity) {
			redistributor_phys[cpu->index] = frame;
			*out                           = frame;
			return true;
		}
		uintptr_t next = redistributor_stride != 0u
		                     ? redistributor_stride
		                     : ((typer & GICR_TYPER_VLPIS) != 0u ? 2u * GICV3_REDIST_STRIDE : GICV3_REDIST_STRIDE);
		if ((typer & GICR_TYPER_LAST) != 0u || next > redistributors_size || offset > redistributors_size - next) break;
		offset += next;
	}
	return false;
}

bool aarch64_gicv3_init_local(struct cpu* cpu) {
	uintptr_t redist;
	if (cpu == NULL || cpu != cpu_current() || cpu->index >= GICV3_MAX_CPUS) return false;
	if (!ready) return false;
	if (local_ready[cpu->index]) return true;
	if (!gicv3_redist_for_cpu(cpu, &redist) || !gicv3_map(redist + GICR_SGI_BASE)) return false;
	uint32_t waker = gicv3_read32(redist + GICR_WAKER);
	gicv3_write32(redist + GICR_WAKER, waker & ~GICR_WAKER_PROCESSOR_SLEEP);
	bool awake = false;
	for (uint32_t attempt = 0u; attempt < 100000u; attempt++) {
		if ((gicv3_read32(redist + GICR_WAKER) & GICR_WAKER_CHILDREN_ASLEEP) == 0u) {
			awake = true;
			break;
		}
	}
	if (!awake) return false;
	uint64_t sre;
	__asm__ volatile("mrs %0, ICC_SRE_EL1" : "=r"(sre));
	sre |= 1u;
	__asm__ volatile("msr ICC_SRE_EL1, %0\n\tisb" : : "r"(sre) : "memory");
	__asm__ volatile("mrs %0, ICC_SRE_EL1" : "=r"(sre));
	if ((sre & 1u) == 0u) return false;
	uintptr_t sgi = redist + GICR_SGI_BASE;
	/* SGIs and PPIs are banked: do not inherit local firmware enables. */
	gicv3_write32(sgi + GICD_ICENABLER, UINT32_MAX);
	__asm__ volatile("dsb sy" : : : "memory");
	if (!gicv3_wait_disabled(sgi, 0u)) return false;
	struct irq_state irq   = spinlock_lock_irqsave(&gicv3_distributor_lock);
	uint32_t         group = gicv3_read32(sgi + GICD_IGROUPR);
	gicv3_write32(sgi + GICD_IGROUPR, group | (1u << GICV3_SCHEDULER_SGI));
	spinlock_unlock_irqrestore(&gicv3_distributor_lock, irq);
	gicv3_write8(sgi + GICD_IPRIORITYR + GICV3_SCHEDULER_SGI, 0x40u);
	gicv3_write32(sgi + GICD_ISENABLER, 1u << GICV3_SCHEDULER_SGI);
	uint64_t ctlr;
	__asm__ volatile("mrs %0, ICC_CTLR_EL1" : "=r"(ctlr));
	ctlr &= ~(1ull << 1u);
	__asm__ volatile(
		"msr ICC_CTLR_EL1, %0\n\tmsr ICC_PMR_EL1, %1\n\tmsr ICC_BPR1_EL1, xzr\n\tmsr ICC_IGRPEN1_EL1, %2\n\tisb"
		:
		: "r"(ctlr), "r"(0xffull), "r"(1ull)
		: "memory");
	local_ready[cpu->index] = true;
	return true;
}

static bool gicv3_source_valid(uint32_t id) {
	if (!ready || id < 16u || id >= GICV3_SPURIOUS_INTID) return false;
	uint32_t count = 32u * ((gicv3_read32(distributor_phys + GICD_TYPER) & 0x1fu) + 1u);
	return id < count;
}

size_t aarch64_gicv3_source_domain_count(void) {
	struct hal_interrupt_source_domain_info domain;
	return aarch64_gicv3_source_domain_at(0u, &domain) ? 1u : 0u;
}

bool aarch64_gicv3_source_domain_at(size_t index, struct hal_interrupt_source_domain_info* out) {
	if (index != 0u || out == NULL || !ready) return false;
	uint32_t count = 32u * ((gicv3_read32(distributor_phys + GICD_TYPER) & 0x1fu) + 1u);
	if (count > GICV3_SPURIOUS_INTID) count = GICV3_SPURIOUS_INTID;
	if (count <= 32u) return false;
	*out = (struct hal_interrupt_source_domain_info){.domain = 0u, .first_source = 32u, .source_count = count - 32u};
	return true;
}

bool aarch64_gicv3_source_resolve(uint64_t controller_address, uint32_t local_source_id,
                                  struct hal_interrupt_source* out_source) {
	if (out_source == NULL || !ready || controller_address != distributor_phys || local_source_id < 32u ||
	    !gicv3_source_valid(local_source_id))
		return false;
	*out_source = (struct hal_interrupt_source){.domain = 0u, .number = local_source_id};
	return true;
}

bool aarch64_gicv3_source_info(const struct hal_interrupt_source* source, struct hal_interrupt_source_info* out) {
	if (source == NULL || out == NULL || source->domain != 0u || source->number < 32u ||
	    !gicv3_source_valid(source->number))
		return false;
	*out = (struct hal_interrupt_source_info){
		.delivery     = {.domain = 0u, .base = source->number, .limit = source->number + 1u},
		.target_kind  = HAL_INTERRUPT_TARGET_ROUTABLE,
		.fixed_target = NULL
    };
	return true;
}

bool aarch64_gicv3_source_target_supported(const struct hal_interrupt_source* source, const struct cpu* target) {
	struct hal_interrupt_source_info info;
	return target != NULL && target->index < GICV3_MAX_CPUS && local_ready[target->index] &&
	       aarch64_gicv3_source_info(source, &info);
}

static bool gicv3_set_enabled(uintptr_t base, uint32_t id, bool enabled) {
	uint32_t offset = (enabled ? GICD_ISENABLER : GICD_ICENABLER) + (id / 32u) * 4u;
	if (!gicv3_map(base + offset)) return false;
	gicv3_write32(base + offset, 1u << (id % 32u));
	__asm__ volatile("dsb sy" : : : "memory");
	return enabled || gicv3_wait_disabled(base, id);
}

static bool gicv3_source_init(struct hal_interrupt_source_state* state, const struct hal_interrupt_source* source,
                              const struct hal_interrupt_delivery* delivery, bool local) {
	struct hal_interrupt_source_info info;
	uintptr_t                        base;
	if (state == NULL || state->initialized || delivery == NULL || delivery->target == NULL ||
	    !aarch64_gicv3_init_global() || delivery->polarity > HAL_INTERRUPT_POLARITY_HIGH ||
	    delivery->trigger > HAL_INTERRUPT_TRIGGER_LEVEL)
		return false;
	if (local) {
		if (source == NULL || source->domain != 0u || source->number < 16u || source->number >= 32u ||
		    !gicv3_source_valid(source->number) || delivery->event.domain != 0u ||
		    delivery->event.id != source->number || delivery->target != cpu_current() ||
		    delivery->trigger != HAL_INTERRUPT_TRIGGER_FIRMWARE)
			return false;
	}
	else if (!aarch64_gicv3_source_info(source, &info) || delivery->event.domain != info.delivery.domain ||
	         delivery->event.id != info.delivery.base ||
	         !aarch64_gicv3_source_target_supported(source, delivery->target))
		return false;
	if (source->number < 32u) {
		uintptr_t redist;
		if (!aarch64_gicv3_init_local(cpu_current()) || !gicv3_redist_for_cpu(delivery->target, &redist)) return false;
		base = redist + GICR_SGI_BASE;
	}
	else base = distributor_phys;
	if (!gicv3_set_enabled(base, source->number, false) ||
	    !gicv3_map(base + GICD_IGROUPR + (source->number / 32u) * 4u) ||
	    !gicv3_map(base + GICD_IPRIORITYR + source->number))
		return false;
	uint32_t group_offset = GICD_IGROUPR + (source->number / 32u) * 4u;
	if (source->number >= 32u && delivery->trigger != HAL_INTERRUPT_TRIGGER_FIRMWARE) {
		uint32_t config_offset = GICD_ICFGR + (source->number / 16u) * 4u;
		if (!gicv3_map(distributor_phys + config_offset)) return false;
	}
	struct irq_state irq = spinlock_lock_irqsave(&gicv3_distributor_lock);
	gicv3_write32(base + group_offset, gicv3_read32(base + group_offset) | (1u << (source->number % 32u)));
	if (source->number >= 32u) {
		if (delivery->trigger != HAL_INTERRUPT_TRIGGER_FIRMWARE) {
			uint32_t config_offset = GICD_ICFGR + (source->number / 16u) * 4u;
			uint32_t config        = gicv3_read32(distributor_phys + config_offset);
			uint32_t edge          = 1u << ((source->number % 16u) * 2u + 1u);
			if (delivery->trigger == HAL_INTERRUPT_TRIGGER_EDGE) config |= edge;
			else config &= ~edge;
			gicv3_write32(distributor_phys + config_offset, config);
		}
	}
	spinlock_unlock_irqrestore(&gicv3_distributor_lock, irq);
	gicv3_write8(base + GICD_IPRIORITYR + source->number, 0x80u);
	if (source->number >= 32u) {
		uint32_t route_offset = GICD_IROUTER + source->number * 8u;
		if (!gicv3_map(distributor_phys + route_offset)) return false;
		uint64_t route = delivery->target->arch_id & 0x000000ff00ffffffull;
		gicv3_write64(distributor_phys + route_offset, route);
	}
	__asm__ volatile("dsb sy\n\tisb" : : : "memory");
	*state = (struct hal_interrupt_source_state){
		.source = *source, .register_base = base, .initialized = true, .masked = true};
	return true;
}

bool aarch64_gicv3_source_init(struct hal_interrupt_source_state* state, const struct hal_interrupt_source* source,
                               const struct hal_interrupt_delivery* delivery) {
	return gicv3_source_init(state, source, delivery, false);
}

bool aarch64_gicv3_local_source_init(struct hal_interrupt_source_state* state, uint32_t id, const struct cpu* target) {
	struct hal_interrupt_source   source   = {.domain = 0u, .number = id};
	struct hal_interrupt_delivery delivery = {
		.target   = target,
		.event    = {.domain = 0u, .id = id},
		.trigger  = HAL_INTERRUPT_TRIGGER_FIRMWARE,
		.polarity = HAL_INTERRUPT_POLARITY_FIRMWARE
    };
	return gicv3_source_init(state, &source, &delivery, true);
}

bool aarch64_gicv3_source_mask(struct hal_interrupt_source_state* state) {
	if (state == NULL || !state->initialized || !gicv3_set_enabled(state->register_base, state->source.number, false))
		return false;
	state->masked = true;
	return true;
}

bool aarch64_gicv3_source_unmask(struct hal_interrupt_source_state* state) {
	if (state == NULL || !state->initialized || !gicv3_set_enabled(state->register_base, state->source.number, true))
		return false;
	state->masked = false;
	return true;
}

bool aarch64_gicv3_source_deinit(struct hal_interrupt_source_state* state) {
	if (state == NULL) return false;
	if (!state->initialized) return true;
	if (!aarch64_gicv3_source_mask(state)) return false;
	state->initialized = false;
	return true;
}

static bool gicv3_mbi_ranges(const uint8_t** out_ranges, size_t* out_count) {
	const uint8_t* ranges;
	size_t         size;
	if (!ready || (gicv3_read32(distributor_phys + GICD_TYPER) & GICD_TYPER_MBIS) == 0u ||
	    !iommu_fdt_controller_property("arm,gic-v3", 0u, "msi-controller", NULL, NULL) ||
	    !iommu_fdt_controller_property("arm,gic-v3", 0u, "mbi-ranges", &ranges, &size) || size == 0u ||
	    size % 8u != 0u || size > (GICV3_SPURIOUS_INTID - 32u) * 8u)
		return false;
	for (size_t index = 0u; index < size / 8u; index++) {
		uint32_t first = iommu_fdt_u32(ranges + index * 8u);
		uint32_t count = iommu_fdt_u32(ranges + index * 8u + 4u);
		if (first < 32u || first >= GICV3_SPURIOUS_INTID || count == 0u || count > GICV3_SPURIOUS_INTID - first ||
		    !gicv3_source_valid(first + count - 1u))
			return false;
		for (size_t previous = 0u; previous < index; previous++) {
			uint32_t other_first = iommu_fdt_u32(ranges + previous * 8u);
			uint32_t other_count = iommu_fdt_u32(ranges + previous * 8u + 4u);
			if (first < other_first + other_count && other_first < first + count) return false;
		}
	}
	*out_ranges = ranges;
	*out_count  = size / 8u;
	return true;
}

size_t aarch64_gicv3_message_range_count(void) {
	const uint8_t* ranges;
	size_t         count;
	return gicv3_mbi_ranges(&ranges, &count) ? count : 0u;
}

bool aarch64_gicv3_message_range_at(size_t index, struct hal_interrupt_message_range* out) {
	const uint8_t* ranges;
	size_t         count;
	if (out == NULL || !gicv3_mbi_ranges(&ranges, &count) || index >= count) return false;
	uint32_t first = iommu_fdt_u32(ranges + index * 8u);
	uint32_t span  = iommu_fdt_u32(ranges + index * 8u + 4u);
	*out           = (struct hal_interrupt_message_range){
		.domain = 0u, .delivery = {.domain = 0u, .base = first, .limit = first + span}
    };
	return true;
}

bool aarch64_gicv3_message_target_supported(uint32_t domain, const struct hal_interrupt_message_source* source,
                                            const struct cpu* target) {
	const uint8_t* ranges;
	size_t         count;
	return domain == 0u && source == NULL && target != NULL && target->index < GICV3_MAX_CPUS &&
	       local_ready[target->index] && gicv3_mbi_ranges(&ranges, &count);
}

bool aarch64_gicv3_message_init(struct hal_interrupt_message_state*         state,
                                const struct hal_interrupt_message_request* request,
                                struct hal_interrupt_message*               out) {
	const uint8_t* ranges;
	size_t         count;
	if (state == NULL || state->initialized || request == NULL || out == NULL ||
	    !aarch64_gicv3_message_target_supported(request->domain, request->source, request->target) ||
	    !gicv3_mbi_ranges(&ranges, &count))
		return false;
	uint32_t id = request->event.id;
	if (request->event.domain != 0u) return false;
	bool in_range = false;
	for (size_t index = 0u; index < count; ++index) {
		uint32_t first = iommu_fdt_u32(ranges + index * 8u);
		uint32_t span  = iommu_fdt_u32(ranges + index * 8u + 4u);
		if (id >= first && id - first < span) {
			in_range = true;
			break;
		}
	}
	if (!in_range) return false;
	uint64_t       message_base = (uint64_t)distributor_phys;
	const uint8_t* alias;
	size_t         alias_size;
	if (iommu_fdt_controller_property("arm,gic-v3", 0u, "mbi-alias", &alias, &alias_size) &&
	    ((alias_size != 4u && alias_size != 8u) ||
	     !iommu_fdt_cells(alias, (uint32_t)(alias_size / 4u), &message_base) || message_base == 0u))
		return false;
	if (message_base > UINT64_MAX - GICD_SETSPI_NSR) return false;
	uintptr_t base          = distributor_phys;
	uint32_t  group_offset  = GICD_IGROUPR + (id / 32u) * 4u;
	uint32_t  config_offset = GICD_ICFGR + (id / 16u) * 4u;
	uint32_t  route_offset  = GICD_IROUTER + id * 8u;
	if (!gicv3_set_enabled(base, id, false) || !gicv3_map(base + group_offset) ||
	    !gicv3_map(base + GICD_IPRIORITYR + id) || !gicv3_map(base + config_offset) || !gicv3_map(base + route_offset))
		return false;
	struct irq_state irq = spinlock_lock_irqsave(&gicv3_distributor_lock);
	gicv3_write32(base + group_offset, gicv3_read32(base + group_offset) | (1u << (id % 32u)));
	uint32_t config = gicv3_read32(base + config_offset);
	config |= 1u << ((id % 16u) * 2u + 1u);
	gicv3_write32(base + config_offset, config);
	spinlock_unlock_irqrestore(&gicv3_distributor_lock, irq);
	gicv3_write8(base + GICD_IPRIORITYR + id, 0x80u);
	gicv3_write64(base + route_offset, request->target->arch_id & 0x000000ff00ffffffull);
	if (!gicv3_set_enabled(base, id, true)) return false;
	*out   = (struct hal_interrupt_message){.address = message_base + GICD_SETSPI_NSR, .data = id};
	*state = (struct hal_interrupt_message_state){.event = request->event, .uses_gicv3 = true, .initialized = true};
	return true;
}

bool aarch64_gicv3_message_deinit(struct hal_interrupt_message_state* state) {
	if (state == NULL || !state->uses_gicv3) return false;
	if (!state->initialized) return true;
	if (!gicv3_set_enabled(distributor_phys, state->event.id, false)) return false;
	state->initialized = false;
	return true;
}

bool aarch64_gicv3_prepare_smp(void) {
	return aarch64_gicv3_ready() && aarch64_gicv3_init_local(cpu_current());
}

bool aarch64_gicv3_handle_irq(const struct exception_frame* frame) {
	if (frame == NULL || (frame->vector & 0x3u) != 1u || !ready) return false;
	uint64_t iar;
	__asm__ volatile("mrs %0, ICC_IAR1_EL1" : "=r"(iar));
	uint32_t intid = (uint32_t)(iar & 0x00ffffffu);
	if (intid >= GICV3_SPURIOUS_INTID && intid < 1024u) return true;
	if (intid == GICV3_TIMER_PPI) (void)aarch64_clock_fire();
	else if (intid != GICV3_SCHEDULER_SGI && gicv3_source_valid(intid)) {
		if (!interrupt_handle_event((struct hal_interrupt_event){.domain = 0u, .id = intid})) {
			uintptr_t base = distributor_phys;
			if (intid < 32u) {
				uintptr_t redist;
				if (gicv3_redist_for_cpu(cpu_current(), &redist)) base = redist + GICR_SGI_BASE;
			}
			(void)gicv3_set_enabled(base, intid, false);
		}
	}
	__asm__ volatile("msr ICC_EOIR1_EL1, %0\n\tisb" : : "r"(iar) : "memory");
	return true;
}
