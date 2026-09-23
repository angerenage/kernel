#include "htvec.h"

#include <core/interrupt.h>
#include <stdbool.h>
#include <stdint.h>

#include "controller.h"
#include "liointc.h"
#include "pch_pic.h"

#define HTVEC_ENABLE 0x20u
#define LIOINTC_ENABLE 0x28u

bool loongarch64_htvec_probe(void) {
	if (!htvec.described) return true;
	if (htvec.ready) return true;
	struct irq_state irq = loongarch64_controllers_lock();
	if (!htvec.ready && loongarch64_map_mmio(htvec.physical_base, htvec.size, &htvec.virtual_base)) {
		for (uint32_t group = 0u; group < HTVEC_GROUP_COUNT; group++) {
			loongarch64_mmio_write32(htvec.virtual_base, HTVEC_ENABLE + group * 4u, 0u);
			loongarch64_mmio_write32(htvec.virtual_base, group * 4u, UINT32_MAX);
		}
		htvec.ready = true;
	}
	loongarch64_controllers_unlock(irq);
	struct fixed_controller* lio = loongarch64_fixed_by_domain(LOONGARCH64_DOMAIN_LIOINTC);
	if (htvec.ready && lio != NULL && loongarch64_liointc_probe(lio)) {
		irq = loongarch64_controllers_lock();
		for (uint32_t group = 0u; group < HTVEC_GROUP_COUNT; group++)
			if (htvec.cascades[group] < lio->source_count)
				loongarch64_mmio_write32(lio->virtual_base, LIOINTC_ENABLE, 1u << htvec.cascades[group]);
		loongarch64_controllers_unlock(irq);
	}
	return htvec.ready;
}

void loongarch64_htvec_set_enabled(uint32_t vector, bool enabled) {
	if (!htvec.ready || vector >= HTVEC_GROUP_COUNT * 32u) return;
	uint32_t offset = HTVEC_ENABLE + (vector / 32u) * 4u;
	uint32_t value  = loongarch64_mmio_read32(htvec.virtual_base, offset);
	if (enabled) value |= 1u << (vector % 32u);
	else value &= ~(1u << (vector % 32u));
	loongarch64_mmio_write32(htvec.virtual_base, offset, value);
}

bool loongarch64_htvec_handle(void) {
	if (!htvec.ready) return false;
	bool handled = false;
	for (uint32_t group = 0u; group < HTVEC_GROUP_COUNT; group++) {
		uint32_t pending = loongarch64_mmio_read32(htvec.virtual_base, group * 4u);
		while (pending != 0u) {
			uint32_t bit    = (uint32_t)__builtin_ctz(pending);
			uint32_t vector = group * 32u + bit;
			if (!interrupt_handle_event(
					(struct hal_interrupt_event){.domain = LOONGARCH64_DELIVERY_DOMAIN_VECTOR, .id = vector}))
				(void)loongarch64_pch_pic_mask_vector_leaf(vector);
			loongarch64_mmio_write32(htvec.virtual_base, group * 4u, 1u << bit);
			pending &= ~(1u << bit);
			handled = true;
		}
	}
	return handled;
}
