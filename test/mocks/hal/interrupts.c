#include <core/cpu.h>
#include <hal/interrupts.h>

static _Thread_local bool hosted_irq_enabled = true;
static const struct cpu*  last_source_target;
static bool               last_source_unmasked_from_masked;
static bool               source_mask_fails;

const struct cpu* hal_interrupt_mock_last_target(void) {
	return last_source_target;
}
bool hal_interrupt_mock_unmasked_from_masked(void) {
	return last_source_unmasked_from_masked;
}
void hal_interrupt_mock_set_mask_failure(bool fail) {
	source_mask_fails = fail;
}

bool irq_enabled(void) {
	return hosted_irq_enabled;
}

void irq_disable_local(void) {
	hosted_irq_enabled = false;
}

void irq_enable_local(void) {
	hosted_irq_enabled = true;
}

bool hal_interrupts_init_global(void) {
	return true;
}

bool hal_interrupts_init_local(struct cpu* cpu) {
	if (cpu == NULL) return false;

	cpu_interrupts_set_ready(cpu, true);
	return true;
}

size_t hal_interrupt_source_domain_count(void) {
	return 1u;
}

bool hal_interrupt_source_domain_at(size_t index, struct hal_interrupt_source_domain_info* out_domain) {
	if (index != 0u || out_domain == NULL) return false;
	*out_domain = (struct hal_interrupt_source_domain_info){.domain = 0u, .first_source = 0u, .source_count = 256u};
	return true;
}

bool hal_interrupt_source_info(const struct hal_interrupt_source* source, struct hal_interrupt_source_info* out_info) {
	if (source == NULL || out_info == NULL || source->domain != 0u || source->number >= 256u) return false;
	*out_info = (struct hal_interrupt_source_info){
		.delivery     = {.domain = 0u, .base = source->number, .limit = source->number + 1u},
		.target_kind  = HAL_INTERRUPT_TARGET_ROUTABLE,
		.fixed_target = NULL
    };
	return true;
}

bool hal_interrupt_source_target_supported(const struct hal_interrupt_source* source, const struct cpu* target) {
	struct hal_interrupt_source_info info;
	return target != NULL && hal_interrupt_source_info(source, &info);
}

bool hal_interrupt_source_init(struct hal_interrupt_source_state* state, const struct hal_interrupt_source* source,
                               const struct hal_interrupt_delivery* delivery) {
	struct hal_interrupt_source_info info;
	if (state == NULL || state->initialized || delivery == NULL || !hal_interrupt_source_info(source, &info) ||
	    delivery->event.domain != info.delivery.domain || delivery->event.id < info.delivery.base ||
	    delivery->event.id >= info.delivery.limit || !hal_interrupt_source_target_supported(source, delivery->target) ||
	    delivery->trigger > HAL_INTERRUPT_TRIGGER_LEVEL || delivery->polarity > HAL_INTERRUPT_POLARITY_LOW)
		return false;
	*state = (struct hal_interrupt_source_state){
		.source = *source, .target = delivery->target, .initialized = true, .masked = true};
	last_source_target = delivery->target;
	return true;
}

bool hal_interrupt_source_mask(struct hal_interrupt_source_state* state) {
	if (state == NULL || !state->initialized || source_mask_fails) return false;
	state->masked = true;
	return true;
}

bool hal_interrupt_source_unmask(struct hal_interrupt_source_state* state) {
	if (state == NULL || !state->initialized) return false;
	last_source_unmasked_from_masked = state->masked;
	state->masked                    = false;
	return true;
}

bool hal_interrupt_source_deinit(struct hal_interrupt_source_state* state) {
	if (state == NULL) return false;
	if (!state->initialized) return true;
	if (!hal_interrupt_source_mask(state)) return false;
	state->initialized = false;
	return true;
}

size_t hal_interrupt_message_range_count(void) {
	return 1u;
}

bool hal_interrupt_message_range_at(size_t index, struct hal_interrupt_message_range* out_range) {
	if (index != 0u || out_range == NULL) return false;
	*out_range = (struct hal_interrupt_message_range){
		.domain = 0u, .delivery = {.domain = 0u, .base = 0u, .limit = 256u}
    };
	return true;
}

bool hal_interrupt_message_target_supported(uint32_t domain, const struct hal_interrupt_message_source* source,
                                            const struct cpu* target) {
	return domain == 0u && source == NULL && target != NULL;
}

bool hal_interrupt_message_init(struct hal_interrupt_message_state*         state,
                                const struct hal_interrupt_message_request* request,
                                struct hal_interrupt_message*               out_message) {
	if (state == NULL || state->initialized || request == NULL || out_message == NULL ||
	    !hal_interrupt_message_target_supported(request->domain, request->source, request->target) ||
	    request->event.domain != 0u || request->event.id >= 256u)
		return false;
	*out_message       = (struct hal_interrupt_message){.address = 0xfee00000u, .data = request->event.id};
	state->initialized = true;
	return true;
}

bool hal_interrupt_message_deinit(struct hal_interrupt_message_state* state) {
	if (state == NULL) return false;
	if (!state->initialized) return true;
	state->initialized = false;
	return true;
}
