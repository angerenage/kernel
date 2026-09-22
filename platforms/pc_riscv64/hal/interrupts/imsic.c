#include "imsic.h"

#include <core/cpu.h>
#include <hal/cpu.h>
#include <hal/interrupts.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../../../iommu_fdt.h"
#include "aplic.h"
#include "interrupts.h"

#define IMSIC_MAX_CPUS 64u
#define IMSIC_MAX_IDS 2047u
#define IMSIC_PAGE_SIZE 0x1000u
#define IMSIC_EIDELIVERY 0x70u
#define IMSIC_EITHRESHOLD 0x72u
#define IMSIC_EIP0 0x80u
#define IMSIC_EIE0 0xc0u

struct imsic_hart {
	uint64_t  hart_id;
	uintptr_t message_address;
};

static struct {
	struct imsic_hart harts[IMSIC_MAX_CPUS];
	size_t            hart_count;
	size_t            controller;
	uint32_t          ids;
	uint32_t          phandle;
	uint32_t          guest_bits;
	uint64_t          enabled[IMSIC_MAX_CPUS][32u];
	uint64_t          clear_pending[IMSIC_MAX_CPUS][32u];
	uint64_t          requested[IMSIC_MAX_CPUS];
	uint64_t          applied[IMSIC_MAX_CPUS];
	bool              local_ready[IMSIC_MAX_CPUS];
	bool              probed;
	bool              ready;
} imsic;
static uint32_t imsic_init_lock;

static bool imsic_find(void) {
	size_t count = iommu_fdt_controllers("riscv,imsics", SIZE_MAX, NULL);
	if (count == 0u || count > 2u) return false;
	bool selected = false;
	for (size_t controller = 0u; controller < count; controller++) {
		const uint8_t* interrupts;
		size_t         interrupts_size;
		if (!iommu_fdt_controller_property(
				"riscv,imsics", controller, "interrupts-extended", &interrupts, &interrupts_size) ||
		    interrupts_size == 0u || interrupts_size % 8u != 0u || interrupts_size > IMSIC_MAX_CPUS * 8u)
			return false;
		uint32_t cause = iommu_fdt_u32(interrupts + 4u);
		if (cause != 9u && cause != 11u) return false;
		for (size_t offset = 0u; offset < interrupts_size; offset += 8u)
			if (iommu_fdt_u32(interrupts + offset + 4u) != cause) return false;
		if (cause != 9u) continue;
		if (selected) return false;
		selected = true;
		uintptr_t base;
		uintptr_t size;
		if (iommu_fdt_controllers_region("riscv,imsics", controller, &base, &size) <= controller || base == 0u ||
		    base % IMSIC_PAGE_SIZE != 0u)
			return false;
		const uint8_t* property;
		size_t         property_size;
		if (!iommu_fdt_controller_property("riscv,imsics", controller, "riscv,num-ids", &property, &property_size) ||
		    property_size != 4u)
			return false;
		uint32_t ids = iommu_fdt_u32(property);
		if (ids < 63u || ids > IMSIC_MAX_IDS) return false;
		uint32_t phandle = 0u;
		if (iommu_fdt_controller_property("riscv,imsics", controller, "phandle", &property, &property_size)) {
			if (property_size != 4u || (phandle = iommu_fdt_u32(property)) == 0u) return false;
		}
		uint32_t guest_bits = 0u;
		if (iommu_fdt_controller_property(
				"riscv,imsics", controller, "riscv,guest-index-bits", &property, &property_size)) {
			if (property_size != 4u || (guest_bits = iommu_fdt_u32(property)) > 7u) return false;
		}
		uint32_t group_bits = 0u;
		if (iommu_fdt_controller_property(
				"riscv,imsics", controller, "riscv,group-index-bits", &property, &property_size)) {
			if (property_size != 4u || (group_bits = iommu_fdt_u32(property)) != 0u) return false;
		}
		/* Multi-group layouts need per-group reg/interrupt mappings; do not guess them. */
		(void)group_bits;
		size_t    hart_count = interrupts_size / 8u;
		uintptr_t stride     = (uintptr_t)IMSIC_PAGE_SIZE << guest_bits;
		if (hart_count > size / stride || base > UINTPTR_MAX - size) return false;
		for (size_t index = 0u; index < hart_count; index++) {
			uint64_t hart;
			if (!riscv64_interrupt_hart_for_phandle(iommu_fdt_u32(interrupts + index * 8u), &hart)) return false;
			for (size_t previous = 0u; previous < index; previous++)
				if (imsic.harts[previous].hart_id == hart) return false;
			imsic.harts[index] = (struct imsic_hart){.hart_id = hart, .message_address = base + index * stride};
		}
		imsic.hart_count = hart_count;
		imsic.controller = controller;
		imsic.ids        = ids;
		imsic.phandle    = phandle;
		imsic.guest_bits = guest_bits;
	}
	return selected;
}

static bool imsic_probe(void) {
	if (__atomic_load_n(&imsic.probed, __ATOMIC_ACQUIRE)) return imsic.ready;
	struct irq_state irq = irq_save_disable();
	while (__atomic_exchange_n(&imsic_init_lock, 1u, __ATOMIC_ACQUIRE) != 0u) __asm__ volatile("nop");
	if (!imsic.probed) {
		__atomic_store_n(&imsic.ready, imsic_find(), __ATOMIC_RELEASE);
		__atomic_store_n(&imsic.probed, true, __ATOMIC_RELEASE);
	}
	__atomic_store_n(&imsic_init_lock, 0u, __ATOMIC_RELEASE);
	irq_restore(irq);
	return imsic.ready;
}

bool riscv64_imsic_selected_controller(size_t* out_controller) {
	if (out_controller == NULL || !imsic_probe()) return false;
	*out_controller = imsic.controller;
	return true;
}

static bool imsic_hart_for_cpu(const struct cpu* cpu, uintptr_t* out_address) {
	if (cpu == NULL || !__atomic_load_n(&imsic.ready, __ATOMIC_ACQUIRE)) return false;
	for (size_t index = 0u; index < imsic.hart_count; index++) {
		if (imsic.harts[index].hart_id != cpu->arch_id) continue;
		if (out_address != NULL) *out_address = imsic.harts[index].message_address;
		return true;
	}
	return false;
}

bool riscv64_imsic_cpu_has_interface(const struct cpu* cpu) {
	return imsic_hart_for_cpu(cpu, NULL);
}

bool riscv64_imsic_is_parent(uint32_t phandle) {
	return phandle != 0u && imsic_probe() && imsic.phandle == phandle;
}

bool riscv64_imsic_target(const struct cpu* cpu, uint32_t id, uint32_t* out_hart_index) {
	if (id == 0u || !imsic_probe() || id > imsic.ids || !imsic_hart_for_cpu(cpu, NULL)) return false;
	for (size_t index = 0u; index < imsic.hart_count; index++) {
		if (imsic.harts[index].hart_id != cpu->arch_id) continue;
		if (out_hart_index != NULL) *out_hart_index = (uint32_t)index;
		return true;
	}
	return false;
}

bool riscv64_imsic_message_range_at(struct hal_interrupt_message_range* out) {
	if (out == NULL || !imsic_probe()) return false;
	*out = (struct hal_interrupt_message_range){
		.domain   = RISCV64_MESSAGE_DOMAIN_IMSIC,
		.delivery = {.domain = RISCV64_DELIVERY_DOMAIN_IMSIC, .base = 1u, .limit = imsic.ids + 1u}
    };
	return true;
}

bool riscv64_imsic_message_target_supported(uint32_t domain, const struct hal_interrupt_message_source* source,
                                            const struct cpu* target) {
	return domain == RISCV64_MESSAGE_DOMAIN_IMSIC && source == NULL && target != NULL &&
	       target->index < IMSIC_MAX_CPUS && target->interrupts_ready && cpu_state_get(target) == CPU_STATE_ONLINE &&
	       __atomic_load_n(&imsic.ready, __ATOMIC_ACQUIRE) && imsic_hart_for_cpu(target, NULL);
}

static void imsic_write_indirect(uint32_t selector, uint64_t value) {
	__asm__ volatile("csrw 0x150, %0" : : "r"((uint64_t)selector) : "memory");
	__asm__ volatile("csrw 0x151, %0" : : "r"(value) : "memory");
}

void riscv64_imsic_sync_local(void) {
	struct cpu* cpu = cpu_current();
	if (cpu == NULL || cpu->index >= IMSIC_MAX_CPUS || !imsic_probe() || !imsic_hart_for_cpu(cpu, NULL)) return;
	struct irq_state irq        = irq_save_disable();
	uint64_t         generation = __atomic_load_n(&imsic.requested[cpu->index], __ATOMIC_ACQUIRE);
	for (uint32_t word = 0u; word <= imsic.ids / 64u; word++)
		imsic_write_indirect(IMSIC_EIE0 + word * 2u,
		                     __atomic_load_n(&imsic.enabled[cpu->index][word], __ATOMIC_ACQUIRE));
	for (uint32_t word = 0u; word <= imsic.ids / 64u; word++) {
		uint64_t clear = __atomic_exchange_n(&imsic.clear_pending[cpu->index][word], 0u, __ATOMIC_ACQ_REL);
		if (clear == 0u) continue;
		__asm__ volatile("csrw 0x150, %0" : : "r"((uint64_t)(IMSIC_EIP0 + word * 2u)) : "memory");
		__asm__ volatile("csrc 0x151, %0" : : "r"(clear) : "memory");
	}
	imsic_write_indirect(IMSIC_EITHRESHOLD, 0u);
	imsic_write_indirect(IMSIC_EIDELIVERY, 1u);
	imsic.local_ready[cpu->index] = true;
	__atomic_store_n(&imsic.applied[cpu->index], generation, __ATOMIC_RELEASE);
	irq_restore(irq);
}

bool riscv64_imsic_set_enabled(const struct cpu* cpu, uint32_t id, bool enabled) {
	if (cpu == NULL || cpu->index >= IMSIC_MAX_CPUS || !cpu->interrupts_ready ||
	    cpu_state_get(cpu) != CPU_STATE_ONLINE || !riscv64_imsic_target(cpu, id, NULL))
		return false;
	uint64_t* word = &imsic.enabled[cpu->index][id / 64u];
	uint64_t  bit  = 1ull << (id % 64u);
	if (enabled) __atomic_fetch_or(word, bit, __ATOMIC_ACQ_REL);
	else {
		__atomic_fetch_and(word, ~bit, __ATOMIC_ACQ_REL);
		__atomic_fetch_or(&imsic.clear_pending[cpu->index][id / 64u], bit, __ATOMIC_ACQ_REL);
	}
	uint64_t generation = __atomic_add_fetch(&imsic.requested[cpu->index], 1u, __ATOMIC_ACQ_REL);
	if (cpu == cpu_current()) riscv64_imsic_sync_local();
	else {
		hal_cpu_kick(cpu);
		/* The target is online and interrupt-ready, so its wake IPI will apply this generation. */
		while (__atomic_load_n(&imsic.applied[cpu->index], __ATOMIC_ACQUIRE) < generation) __asm__ volatile("nop");
	}
	return true;
}

bool riscv64_imsic_message_init(struct hal_interrupt_message_state*         state,
                                const struct hal_interrupt_message_request* request,
                                struct hal_interrupt_message*               out_message) {
	struct hal_interrupt_message_range range;
	uintptr_t                          address;
	if (state == NULL || state->initialized || request == NULL || out_message == NULL ||
	    !riscv64_imsic_message_target_supported(request->domain, request->source, request->target) ||
	    !riscv64_imsic_message_range_at(&range) || request->event.domain != range.delivery.domain ||
	    request->event.id < range.delivery.base || request->event.id >= range.delivery.limit ||
	    !imsic_hart_for_cpu(request->target, &address))
		return false;
	if (!riscv64_imsic_set_enabled(request->target, request->event.id, true)) return false;
	*out_message = (struct hal_interrupt_message){.address = address, .data = request->event.id};
	*state =
		(struct hal_interrupt_message_state){.target = request->target, .event = request->event, .initialized = true};
	return true;
}

bool riscv64_imsic_message_deinit(struct hal_interrupt_message_state* state) {
	if (state == NULL) return false;
	if (!state->initialized) return true;
	if (state->event.domain != RISCV64_DELIVERY_DOMAIN_IMSIC ||
	    !riscv64_imsic_set_enabled(state->target, state->event.id, false))
		return false;
	state->initialized = false;
	return true;
}

bool riscv64_imsic_handle_external_irq(void) {
	struct cpu* cpu = cpu_current();
	if (cpu == NULL || cpu->index >= IMSIC_MAX_CPUS || !imsic.local_ready[cpu->index] || !imsic_hart_for_cpu(cpu, NULL))
		return false;
	uint64_t top;
	__asm__ volatile("csrrw %0, 0x15c, zero" : "=r"(top) : : "memory");
	uint32_t id = (uint32_t)(top >> 16u);
	if (id == 0u) return false;
	(void)riscv64_aplic_handle_message_id(id);
	return true;
}
