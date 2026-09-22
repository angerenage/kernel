#include "pch_pic.h"

#include <hal/interrupts.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "controller.h"
#include "eiointc.h"
#include "htvec.h"
#include "pch_lpc.h"

#define PCH_PIC_ID 0x04u
#define PCH_PIC_MASK 0x20u
#define PCH_PIC_HTMSI_EN 0x40u
#define PCH_PIC_EDGE 0x60u
#define PCH_PIC_CLEAR 0x80u
#define PCH_PIC_AUTO0 0xc0u
#define PCH_PIC_AUTO1 0xe0u
#define PCH_PIC_ROUTE(source) (0x100u + (source))
#define PCH_PIC_VECTOR(source) (0x200u + (source))
#define PCH_PIC_POLARITY 0x3e0u

bool loongarch64_pch_pic_probe(struct fixed_controller* controller) {
	if (controller == NULL || controller->kind != FIXED_PCH_PIC) return false;
	if (controller->ready) return true;
	struct irq_state irq = loongarch64_controllers_lock();
	if (controller->ready) goto done;
	if (!loongarch64_map_mmio(controller->physical_base, controller->size, &controller->virtual_base)) goto done;
	uint32_t source_count = ((loongarch64_mmio_read32(controller->virtual_base, PCH_PIC_ID) >> 16u) & 0xffu) + 1u;
	if (source_count > PCH_PIC_SOURCE_COUNT) goto done;
	controller->source_count = source_count;
	for (uint32_t word = 0u; word < (source_count + 31u) / 32u; word++) {
		loongarch64_mmio_write32(controller->virtual_base, PCH_PIC_MASK + word * 4u, UINT32_MAX);
		loongarch64_mmio_write32(controller->virtual_base, PCH_PIC_CLEAR + word * 4u, UINT32_MAX);
		loongarch64_mmio_write32(controller->virtual_base, PCH_PIC_AUTO0 + word * 4u, 0u);
		loongarch64_mmio_write32(controller->virtual_base, PCH_PIC_AUTO1 + word * 4u, 0u);
		loongarch64_mmio_write32(controller->virtual_base, PCH_PIC_HTMSI_EN + word * 4u, UINT32_MAX);
	}
	for (uint32_t source = 0u; source < controller->source_count; source++) {
		loongarch64_mmio_write8(
			controller->virtual_base, PCH_PIC_VECTOR(source), (uint8_t)(controller->vector_base + source));
		loongarch64_mmio_write8(controller->virtual_base, PCH_PIC_ROUTE(source), 1u);
	}
	controller->ready = true;
done:
	loongarch64_controllers_unlock(irq);
	return controller->ready;
}

bool loongarch64_pch_pic_source_reserved(const struct fixed_controller* controller, uint32_t source) {
	if (controller == NULL || controller->domain != LOONGARCH64_DOMAIN_PCH_PIC_BASE) return false;
	for (size_t index = 0u; index < fixed_controller_count; index++)
		if (fixed_controllers[index].kind == FIXED_PCH_LPC && fixed_controllers[index].cascade == source) return true;
	return false;
}

bool loongarch64_pch_pic_set_masked_route(struct fixed_controller* controller, uint32_t source, uint32_t route,
                                          bool masked) {
	if (controller == NULL || source >= controller->source_count || route >= EIOINTC_VECTOR_COUNT ||
	    !loongarch64_pch_pic_probe(controller))
		return false;
	struct irq_state irq    = loongarch64_controllers_lock();
	uint32_t         offset = PCH_PIC_MASK + (source / 32u) * 4u;
	uint32_t         value  = loongarch64_mmio_read32(controller->virtual_base, offset);
	if (masked) value |= 1u << (source % 32u);
	else {
		loongarch64_mmio_write32(controller->virtual_base, PCH_PIC_CLEAR + (source / 32u) * 4u, 1u << (source % 32u));
		value &= ~(1u << (source % 32u));
	}
	loongarch64_mmio_write32(controller->virtual_base, offset, value);
	loongarch64_eiointc_set_enabled(route, !masked);
	loongarch64_htvec_set_enabled(route, !masked);
	loongarch64_controllers_unlock(irq);
	return true;
}

bool loongarch64_pch_pic_set_masked(struct fixed_controller* controller, uint32_t source, bool masked) {
	return controller != NULL &&
	       loongarch64_pch_pic_set_masked_route(controller, source, controller->vector_base + source, masked);
}

bool loongarch64_pch_pic_configure(struct fixed_controller* controller, uint32_t source, uint32_t route,
                                   const struct hal_interrupt_delivery* delivery) {
	if (controller == NULL || delivery == NULL || source >= controller->source_count ||
	    !loongarch64_pch_pic_probe(controller))
		return false;
	struct irq_state irq = loongarch64_controllers_lock();
	loongarch64_mmio_write8(controller->virtual_base, PCH_PIC_VECTOR(source), (uint8_t)route);
	uint32_t word = (source / 32u) * 4u;
	uint32_t bit  = 1u << (source % 32u);
	if (delivery->trigger != HAL_INTERRUPT_TRIGGER_FIRMWARE) {
		uint32_t edge = loongarch64_mmio_read32(controller->virtual_base, PCH_PIC_EDGE + word);
		if (delivery->trigger == HAL_INTERRUPT_TRIGGER_EDGE) edge |= bit;
		else edge &= ~bit;
		loongarch64_mmio_write32(controller->virtual_base, PCH_PIC_EDGE + word, edge);
	}
	if (delivery->polarity != HAL_INTERRUPT_POLARITY_FIRMWARE) {
		uint32_t polarity = loongarch64_mmio_read32(controller->virtual_base, PCH_PIC_POLARITY + word);
		if (delivery->polarity == HAL_INTERRUPT_POLARITY_LOW) polarity |= bit;
		else polarity &= ~bit;
		loongarch64_mmio_write32(controller->virtual_base, PCH_PIC_POLARITY + word, polarity);
	}
	loongarch64_eiointc_route(route, delivery->target);
	loongarch64_controllers_unlock(irq);
	return true;
}

bool loongarch64_pch_pic_mask_vector_leaf(uint32_t vector) {
	for (size_t index = 0u; index < fixed_controller_count; index++) {
		struct fixed_controller* controller = &fixed_controllers[index];
		if (controller->kind != FIXED_PCH_PIC || !loongarch64_pch_pic_probe(controller)) continue;
		for (uint32_t source = 0u; source < controller->source_count; ++source) {
			if (loongarch64_mmio_read8(controller->virtual_base, PCH_PIC_VECTOR(source)) != (uint8_t)vector) continue;
			if (loongarch64_pch_lpc_handle_cascade(source)) return true;
			if (!loongarch64_pch_pic_set_masked_route(controller, source, vector, true)) return false;
			loongarch64_mmio_write32(
				controller->virtual_base, PCH_PIC_CLEAR + (source / 32u) * 4u, 1u << (source % 32u));
			return true;
		}
	}
	return false;
}
