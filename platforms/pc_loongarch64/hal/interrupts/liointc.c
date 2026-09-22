#include "liointc.h"

#include <core/cpu.h>
#include <hal/interrupts.h>
#include <stdbool.h>
#include <stdint.h>

#include "controller.h"
#include "htvec.h"

#define LIOINTC_ENABLE 0x28u
#define LIOINTC_DISABLE 0x2cu
#define LIOINTC_POLARITY 0x30u
#define LIOINTC_EDGE 0x34u
#define LIOINTC_STATUS(core) (0x40u + (core) * 8u)

bool loongarch64_liointc_probe(struct fixed_controller* controller) {
	if (controller == NULL || controller->kind != FIXED_LIOINTC) return false;
	if (controller->ready) return true;
	struct irq_state irq = loongarch64_controllers_lock();
	if (controller->ready) goto done;
	if (!loongarch64_map_mmio(controller->physical_base, controller->size, &controller->virtual_base)) goto done;
	loongarch64_mmio_write32(controller->virtual_base, LIOINTC_DISABLE, UINT32_MAX);
	loongarch64_mmio_write32(controller->virtual_base, LIOINTC_EDGE, 0u);
	for (uint32_t source = 0u; source < LIOINTC_SOURCE_COUNT; source++) {
		uint8_t route = 1u;
		for (uint32_t parent = 0u; parent < 2u; parent++)
			if ((lio_cascade_maps[parent] & (1u << source)) != 0u) route |= (uint8_t)(1u << (parent + 4u));
		loongarch64_mmio_write8(controller->virtual_base, source, route);
	}
	controller->ready = true;
done:
	loongarch64_controllers_unlock(irq);
	return controller->ready;
}

bool loongarch64_liointc_source_reserved(uint32_t source) {
	if (!htvec.described) return false;
	for (uint32_t group = 0u; group < HTVEC_GROUP_COUNT; group++)
		if (htvec.cascades[group] == source) return true;
	return false;
}

bool loongarch64_liointc_set_masked(struct fixed_controller* controller, uint32_t source, bool masked) {
	if (controller == NULL || source >= controller->source_count || !loongarch64_liointc_probe(controller))
		return false;
	struct irq_state irq = loongarch64_controllers_lock();
	loongarch64_mmio_write32(controller->virtual_base, masked ? LIOINTC_DISABLE : LIOINTC_ENABLE, 1u << source);
	loongarch64_controllers_unlock(irq);
	return true;
}

bool loongarch64_liointc_configure(struct fixed_controller* controller, uint32_t source,
                                   const struct hal_interrupt_delivery* delivery) {
	if (controller == NULL || delivery == NULL || source >= controller->source_count ||
	    !loongarch64_liointc_probe(controller))
		return false;
	struct irq_state irq = loongarch64_controllers_lock();
	uint32_t         bit = 1u << source;
	if (delivery->trigger != HAL_INTERRUPT_TRIGGER_FIRMWARE) {
		uint32_t edge = loongarch64_mmio_read32(controller->virtual_base, LIOINTC_EDGE);
		if (delivery->trigger == HAL_INTERRUPT_TRIGGER_EDGE) edge |= bit;
		else edge &= ~bit;
		loongarch64_mmio_write32(controller->virtual_base, LIOINTC_EDGE, edge);
	}
	if (delivery->polarity != HAL_INTERRUPT_POLARITY_FIRMWARE) {
		uint32_t polarity = loongarch64_mmio_read32(controller->virtual_base, LIOINTC_POLARITY);
		if (delivery->polarity == HAL_INTERRUPT_POLARITY_LOW) polarity |= bit;
		else polarity &= ~bit;
		loongarch64_mmio_write32(controller->virtual_base, LIOINTC_POLARITY, polarity);
	}
	loongarch64_controllers_unlock(irq);
	return true;
}

bool loongarch64_liointc_handle(uint64_t pending_cpu) {
	struct fixed_controller* lio = loongarch64_fixed_by_domain(LOONGARCH64_DOMAIN_LIOINTC);
	if (lio == NULL || !loongarch64_liointc_probe(lio)) return false;
	bool cascade_pending = false;
	for (uint32_t parent = 0u; parent < 2u; parent++)
		if (lio_cascades[parent] >= LOONGARCH64_CPU_HWI_BASE && lio_cascades[parent] < LOONGARCH64_CPU_HWI_LIMIT &&
		    (pending_cpu & (1ull << lio_cascades[parent])) != 0u)
			cascade_pending = true;
	if (!cascade_pending) return false;
	uint32_t core    = cpu_current() == NULL ? 0u : (uint32_t)(cpu_current()->arch_id & 3u);
	uint32_t pending = loongarch64_mmio_read32(lio->virtual_base, LIOINTC_STATUS(core));
	if (pending == 0u) return false;
	bool handled = false;
	if (htvec.described && htvec.ready) {
		bool ht_pending = false;
		for (uint32_t group = 0u; group < HTVEC_GROUP_COUNT; group++) {
			if (htvec.cascades[group] >= lio->source_count) continue;
			uint32_t bit = 1u << htvec.cascades[group];
			if ((pending & bit) == 0u) continue;
			ht_pending = true;
			pending &= ~bit;
		}
		if (ht_pending) handled = loongarch64_htvec_handle();
	}
	if (pending != 0u) {
		loongarch64_mmio_write32(lio->virtual_base, LIOINTC_DISABLE, pending);
		handled = true;
	}
	return handled;
}
