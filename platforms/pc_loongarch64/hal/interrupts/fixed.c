#include "fixed.h"

#include <core/cpu.h>
#include <hal/interrupts.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "controller.h"
#include "htvec.h"
#include "liointc.h"
#include "pch_lpc.h"
#include "pch_pic.h"

static bool fixed_probe(struct fixed_controller* controller) {
	if (controller == NULL) return false;
	switch (controller->kind) {
	case FIXED_LIOINTC:
		return loongarch64_liointc_probe(controller);
	case FIXED_PCH_PIC:
		return loongarch64_pch_pic_probe(controller);
	case FIXED_PCH_LPC:
		return loongarch64_pch_lpc_probe(controller);
	}
	return false;
}

size_t loongarch64_fixed_source_domain_count(void) {
	size_t count = 0u;
	for (size_t controller_index = 0u; controller_index < fixed_controller_count; controller_index++)
		if (fixed_probe(&fixed_controllers[controller_index])) count++;
	return count;
}

bool loongarch64_fixed_source_domain_at(size_t index, struct hal_interrupt_source_domain_info* out_domain) {
	if (out_domain == NULL) return false;
	for (size_t controller_index = 0u; controller_index < fixed_controller_count; controller_index++) {
		struct fixed_controller* controller = &fixed_controllers[controller_index];
		if (!fixed_probe(controller)) continue;
		if (index != 0u) {
			index--;
			continue;
		}
		*out_domain = (struct hal_interrupt_source_domain_info){
			.domain = controller->domain, .first_source = 0u, .source_count = controller->source_count};
		return true;
	}
	return false;
}

static bool fixed_source_reserved(const struct fixed_controller* controller, uint32_t source) {
	if (controller->kind == FIXED_LIOINTC) return loongarch64_liointc_source_reserved(source);
	if (controller->kind == FIXED_PCH_PIC) return loongarch64_pch_pic_source_reserved(controller, source);
	return false;
}

bool loongarch64_fixed_source_resolve(uint64_t controller_address, uint32_t local_source_id,
                                      struct hal_interrupt_source* out_source) {
	if (out_source == NULL || controller_address > UINTPTR_MAX) return false;
	for (size_t index = 0u; index < fixed_controller_count; index++) {
		struct fixed_controller* controller = &fixed_controllers[index];
		if (controller->physical_base != (uintptr_t)controller_address || !fixed_probe(controller) ||
		    local_source_id >= controller->source_count)
			continue;
		*out_source = (struct hal_interrupt_source){.domain = controller->domain, .number = local_source_id};
		return true;
	}
	return false;
}

bool loongarch64_fixed_source_claimable(const struct hal_interrupt_source* source) {
	if (source == NULL) return false;
	struct fixed_controller* controller = loongarch64_fixed_by_domain(source->domain);
	return controller != NULL && fixed_probe(controller) && source->number < controller->source_count &&
	       !fixed_source_reserved(controller, source->number);
}

bool loongarch64_fixed_source_configuration_supported(const struct hal_interrupt_source* source,
                                                      enum hal_interrupt_trigger         trigger,
                                                      enum hal_interrupt_polarity        polarity) {
	struct hal_interrupt_source_info info;
	if (source == NULL || trigger > HAL_INTERRUPT_TRIGGER_LEVEL || polarity > HAL_INTERRUPT_POLARITY_LOW ||
	    !loongarch64_fixed_source_info(source, &info))
		return false;

	struct fixed_controller* controller = loongarch64_fixed_by_domain(source->domain);
	return controller != NULL && !(controller->kind == FIXED_PCH_LPC && trigger == HAL_INTERRUPT_TRIGGER_EDGE);
}

bool loongarch64_fixed_source_info(const struct hal_interrupt_source* source,
                                   struct hal_interrupt_source_info*  out_info) {
	if (source == NULL || out_info == NULL) return false;
	struct fixed_controller* controller = loongarch64_fixed_by_domain(source->domain);
	if (controller == NULL || !fixed_probe(controller) || source->number >= controller->source_count ||
	    (controller->kind == FIXED_PCH_PIC && !loongarch64_htvec_probe()))
		return false;
	struct hal_interrupt_delivery_range delivery;
	if (controller->kind == FIXED_PCH_PIC) {
		delivery = (struct hal_interrupt_delivery_range){.domain = LOONGARCH64_DELIVERY_DOMAIN_VECTOR,
		                                                 .base   = 0u,
		                                                 .limit  = eiointc.described ? eiointc.vector_count
		                                                                             : EIOINTC_VECTOR_COUNT};
	}
	else {
		delivery = (struct hal_interrupt_delivery_range){.domain = controller->kind == FIXED_LIOINTC
		                                                               ? LOONGARCH64_DELIVERY_DOMAIN_LIOINTC
		                                                               : LOONGARCH64_DELIVERY_DOMAIN_PCH_LPC,
		                                                 .base   = source->number,
		                                                 .limit  = source->number + 1u};
	}
	bool              routable     = controller->kind == FIXED_PCH_PIC && eiointc.described;
	const struct cpu* fixed_target = routable ? NULL : loongarch64_fixed_target;
	if (!routable && fixed_target == NULL) return false;
	*out_info = (struct hal_interrupt_source_info){.delivery     = delivery,
	                                               .target_kind  = routable ? HAL_INTERRUPT_TARGET_ROUTABLE
	                                                                        : HAL_INTERRUPT_TARGET_FIXED,
	                                               .fixed_target = fixed_target};
	return true;
}

bool loongarch64_fixed_source_target_supported(const struct hal_interrupt_source* source, const struct cpu* target) {
	if (source == NULL || target == NULL || target->index >= 64u || !target->interrupts_ready) return false;
	struct fixed_controller* controller = loongarch64_fixed_by_domain(source->domain);
	if (controller == NULL || !controller->ready || source->number >= controller->source_count ||
	    fixed_source_reserved(controller, source->number) ||
	    (controller->kind == FIXED_PCH_PIC && htvec.described && !htvec.ready))
		return false;
	if (controller->kind == FIXED_PCH_PIC && eiointc.described) return target->arch_id < 64u;
	return target == loongarch64_fixed_target;
}

static bool fixed_set_masked(struct fixed_controller* controller, uint32_t source, uint32_t route, bool masked) {
	if (controller == NULL) return false;
	switch (controller->kind) {
	case FIXED_LIOINTC:
		return loongarch64_liointc_set_masked(controller, source, masked);
	case FIXED_PCH_PIC:
		return loongarch64_pch_pic_set_masked_route(controller, source, route, masked);
	case FIXED_PCH_LPC:
		return loongarch64_pch_lpc_set_masked(controller, source, masked);
	}
	return false;
}

static bool fixed_configure(struct fixed_controller* controller, uint32_t source, uint32_t route,
                            const struct hal_interrupt_delivery* delivery) {
	switch (controller->kind) {
	case FIXED_LIOINTC:
		return loongarch64_liointc_configure(controller, source, delivery);
	case FIXED_PCH_PIC:
		return loongarch64_pch_pic_configure(controller, source, route, delivery);
	case FIXED_PCH_LPC:
		return loongarch64_pch_lpc_configure(controller, source, delivery);
	}
	return false;
}

bool loongarch64_fixed_source_init(struct hal_interrupt_source_state* state, const struct hal_interrupt_source* source,
                                   const struct hal_interrupt_delivery* delivery) {
	struct hal_interrupt_source_info info;
	if (state == NULL || state->initialized || source == NULL || delivery == NULL ||
	    !loongarch64_fixed_source_info(source, &info) || delivery->event.domain != info.delivery.domain ||
	    delivery->event.id < info.delivery.base || delivery->event.id >= info.delivery.limit ||
	    delivery->trigger > HAL_INTERRUPT_TRIGGER_LEVEL || delivery->polarity > HAL_INTERRUPT_POLARITY_LOW ||
	    !loongarch64_fixed_source_target_supported(source, delivery->target))
		return false;
	struct fixed_controller* controller = loongarch64_fixed_by_domain(source->domain);
	if (controller == NULL || (controller->kind == FIXED_PCH_LPC && delivery->trigger == HAL_INTERRUPT_TRIGGER_EDGE))
		return false;
	uint32_t route = delivery->event.id;
	if (!fixed_set_masked(controller, source->number, route, true)) return false;
	if (!fixed_configure(controller, source->number, route, delivery)) return false;
	*state =
		(struct hal_interrupt_source_state){.source = *source, .route = route, .initialized = true, .masked = true};
	return true;
}

bool loongarch64_fixed_source_mask(struct hal_interrupt_source_state* state) {
	if (state == NULL || !state->initialized ||
	    !fixed_set_masked(loongarch64_fixed_by_domain(state->source.domain), state->source.number, state->route, true))
		return false;
	state->masked = true;
	return true;
}

bool loongarch64_fixed_source_unmask(struct hal_interrupt_source_state* state) {
	if (state == NULL || !state->initialized ||
	    !fixed_set_masked(loongarch64_fixed_by_domain(state->source.domain), state->source.number, state->route, false))
		return false;
	state->masked = false;
	return true;
}

bool loongarch64_fixed_source_deinit(struct hal_interrupt_source_state* state) {
	if (state == NULL) return false;
	if (!state->initialized) return true;
	if (!loongarch64_fixed_source_mask(state)) return false;
	state->initialized = false;
	return true;
}
