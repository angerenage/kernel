#include "pch_msi.h"

#include <core/cpu.h>
#include <hal/interrupts.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "controller.h"
#include "eiointc.h"
#include "htvec.h"

size_t loongarch64_pch_msi_range_count(void) {
	return pch_msi.described ? 1u : 0u;
}

bool loongarch64_pch_msi_range_at(size_t index, struct hal_interrupt_message_range* out_range) {
	if (index != 0u || out_range == NULL || !pch_msi.described) return false;
	*out_range = (struct hal_interrupt_message_range){
		.domain   = LOONGARCH64_MESSAGE_DOMAIN_PCH_MSI,
		.delivery = {.domain = LOONGARCH64_DELIVERY_DOMAIN_VECTOR,
	                 .base   = pch_msi.first,
	                 .limit  = pch_msi.first + pch_msi.count}
    };
	return true;
}

bool loongarch64_pch_msi_target_supported(uint32_t domain, const struct hal_interrupt_message_source* source,
                                          const struct cpu* target) {
	if (domain != LOONGARCH64_MESSAGE_DOMAIN_PCH_MSI || source != NULL || target == NULL || target->index >= 64u ||
	    !target->interrupts_ready || !pch_msi.described || (!eiointc.described && !htvec.described))
		return false;
	return eiointc.described ? target->arch_id < 64u : target == loongarch64_fixed_target;
}

bool loongarch64_pch_msi_init(struct hal_interrupt_message_state*         state,
                              const struct hal_interrupt_message_request* request,
                              struct hal_interrupt_message*               out_message) {
	struct hal_interrupt_message_range range;
	if (state == NULL || state->initialized || request == NULL || out_message == NULL ||
	    !loongarch64_pch_msi_target_supported(request->domain, request->source, request->target) ||
	    !loongarch64_pch_msi_range_at(0u, &range) || request->event.domain != range.delivery.domain ||
	    request->event.id < range.delivery.base || request->event.id >= range.delivery.limit ||
	    (htvec.described && !loongarch64_htvec_probe()))
		return false;
	uint32_t         vector = request->event.id;
	struct irq_state irq    = loongarch64_controllers_lock();
	loongarch64_eiointc_route(vector, request->target);
	loongarch64_eiointc_set_enabled(vector, true);
	loongarch64_htvec_set_enabled(vector, true);
	loongarch64_controllers_unlock(irq);
	*out_message = (struct hal_interrupt_message){.address = pch_msi.address, .data = vector};
	*state       = (struct hal_interrupt_message_state){
		.domain = LOONGARCH64_MESSAGE_DOMAIN_PCH_MSI, .event = request->event, .initialized = true};
	return true;
}

bool loongarch64_pch_msi_deinit(struct hal_interrupt_message_state* state) {
	if (state == NULL) return false;
	if (!state->initialized) return true;
	if (state->domain != LOONGARCH64_MESSAGE_DOMAIN_PCH_MSI ||
	    state->event.domain != LOONGARCH64_DELIVERY_DOMAIN_VECTOR)
		return false;
	struct irq_state irq = loongarch64_controllers_lock();
	loongarch64_eiointc_set_enabled(state->event.id, false);
	loongarch64_htvec_set_enabled(state->event.id, false);
	loongarch64_controllers_unlock(irq);
	state->initialized = false;
	return true;
}
