#include "pch_lpc.h"

#include <core/interrupt.h>
#include <hal/interrupts.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "controller.h"
#include "pch_pic.h"

#define LPC_CONTROL 0x00u
#define LPC_ENABLE 0x04u
#define LPC_STATUS 0x08u
#define LPC_CLEAR 0x0cu
#define LPC_POLARITY 0x10u
#define LPC_CONTROL_ENABLE (1u << 31)

bool loongarch64_pch_lpc_probe(struct fixed_controller* controller) {
	if (controller == NULL || controller->kind != FIXED_PCH_LPC) return false;
	if (controller->ready) return true;
	struct irq_state irq = loongarch64_controllers_lock();
	if (controller->ready) goto done;
	if (!loongarch64_map_mmio(controller->physical_base, controller->size, &controller->virtual_base)) goto done;
	loongarch64_mmio_write32(controller->virtual_base, LPC_ENABLE, 0u);
	loongarch64_mmio_write32(controller->virtual_base, LPC_CLEAR, (1u << LPC_SOURCE_COUNT) - 1u);
	loongarch64_mmio_write32(controller->virtual_base,
	                         LPC_CONTROL,
	                         loongarch64_mmio_read32(controller->virtual_base, LPC_CONTROL) | LPC_CONTROL_ENABLE);
	controller->ready = true;
done:
	loongarch64_controllers_unlock(irq);
	return controller->ready;
}

bool loongarch64_pch_lpc_set_masked(struct fixed_controller* controller, uint32_t source, bool masked) {
	if (controller == NULL || source >= controller->source_count || !loongarch64_pch_lpc_probe(controller))
		return false;
	struct irq_state irq   = loongarch64_controllers_lock();
	uint32_t         value = loongarch64_mmio_read32(controller->virtual_base, LPC_ENABLE);
	if (masked) value &= ~(1u << source);
	else {
		loongarch64_mmio_write32(controller->virtual_base, LPC_CLEAR, 1u << source);
		value |= 1u << source;
	}
	loongarch64_mmio_write32(controller->virtual_base, LPC_ENABLE, value);
	loongarch64_controllers_unlock(irq);
	if (!masked) {
		struct fixed_controller* parent = loongarch64_fixed_by_domain(LOONGARCH64_DOMAIN_PCH_PIC_BASE);
		if (parent == NULL || controller->cascade >= parent->source_count ||
		    !loongarch64_pch_pic_set_masked(parent, controller->cascade, false)) {
			(void)loongarch64_pch_lpc_set_masked(controller, source, true);
			return false;
		}
	}
	return true;
}

bool loongarch64_pch_lpc_configure(struct fixed_controller* controller, uint32_t source,
                                   const struct hal_interrupt_delivery* delivery) {
	if (controller == NULL || delivery == NULL || source >= controller->source_count ||
	    !loongarch64_pch_lpc_probe(controller))
		return false;
	if (delivery->polarity == HAL_INTERRUPT_POLARITY_FIRMWARE) return true;
	struct irq_state irq      = loongarch64_controllers_lock();
	uint32_t         polarity = loongarch64_mmio_read32(controller->virtual_base, LPC_POLARITY);
	if (delivery->polarity == HAL_INTERRUPT_POLARITY_HIGH) polarity |= 1u << source;
	else polarity &= ~(1u << source);
	loongarch64_mmio_write32(controller->virtual_base, LPC_POLARITY, polarity);
	loongarch64_controllers_unlock(irq);
	return true;
}

bool loongarch64_pch_lpc_handle_cascade(uint32_t parent_source) {
	for (size_t index = 0u; index < fixed_controller_count; index++) {
		struct fixed_controller* controller = &fixed_controllers[index];
		if (controller->kind != FIXED_PCH_LPC || controller->cascade != parent_source ||
		    !loongarch64_pch_lpc_probe(controller))
			continue;
		uint32_t pending = loongarch64_mmio_read32(controller->virtual_base, LPC_STATUS) &
		                   loongarch64_mmio_read32(controller->virtual_base, LPC_ENABLE) &
		                   ((1u << LPC_SOURCE_COUNT) - 1u);
		if (pending == 0u) continue;
		uint32_t unhandled = pending;
		for (uint32_t bits = pending; bits != 0u; bits &= bits - 1u) {
			uint32_t source = (uint32_t)__builtin_ctz(bits);
			if (interrupt_handle_event(
					(struct hal_interrupt_event){.domain = LOONGARCH64_DELIVERY_DOMAIN_PCH_LPC, .id = source}))
				unhandled &= ~(1u << source);
		}
		if (unhandled != 0u)
			loongarch64_mmio_write32(controller->virtual_base,
			                         LPC_ENABLE,
			                         loongarch64_mmio_read32(controller->virtual_base, LPC_ENABLE) & ~unhandled);
		loongarch64_mmio_write32(controller->virtual_base, LPC_CLEAR, pending);
		return true;
	}
	return false;
}
