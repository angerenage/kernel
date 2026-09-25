#include <base/interrupt.h>
#include <core/cpu.h>
#include <hal/interrupts.h>

static _Thread_local bool                 hosted_irq_enabled = true;
static const struct cpu*                  last_source_target;
static bool                               last_source_unmasked_from_masked;
static size_t                             source_unmask_count;
static bool                               source_mask_fails;
static bool                               source_unmask_fails;
static enum hal_interrupt_trigger         last_source_trigger;
static enum hal_interrupt_polarity        last_source_polarity;
static struct hal_interrupt_message_range message_ranges[4] = {
	{.domain = 0u, .delivery = {.domain = 0u, .base = 0u, .limit = 256u}}
};
static size_t                              message_range_count = 1u;
static bool                                message_source_supported;
static bool                                last_message_had_source;
static struct hal_interrupt_message_source last_message_source;

const struct cpu* hal_interrupt_mock_last_target(void) {
	return last_source_target;
}
bool hal_interrupt_mock_unmasked_from_masked(void) {
	return last_source_unmasked_from_masked;
}
size_t hal_interrupt_mock_unmask_count(void) {
	return source_unmask_count;
}
void hal_interrupt_mock_set_mask_failure(bool fail) {
	source_mask_fails = fail;
}
void hal_interrupt_mock_set_unmask_failure(bool fail) {
	source_unmask_fails = fail;
}
enum hal_interrupt_trigger hal_interrupt_mock_last_trigger(void) {
	return last_source_trigger;
}
enum hal_interrupt_polarity hal_interrupt_mock_last_polarity(void) {
	return last_source_polarity;
}
void hal_interrupt_mock_set_message_ranges(const struct hal_interrupt_message_range* ranges, size_t count) {
	if (count > sizeof(message_ranges) / sizeof(message_ranges[0]))
		count = sizeof(message_ranges) / sizeof(message_ranges[0]);
	for (size_t index = 0u; index < count; index++) message_ranges[index] = ranges[index];
	message_range_count = count;
}
void hal_interrupt_mock_set_message_source_supported(bool supported) {
	message_source_supported = supported;
}
bool hal_interrupt_mock_last_message_source(struct hal_interrupt_message_source* out_source) {
	if (!last_message_had_source || out_source == NULL) return false;
	*out_source = last_message_source;
	return true;
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

bool hal_interrupt_source_resolve(uint64_t controller_register_address, uint32_t local_source_id,
                                  struct hal_interrupt_source* out_source) {
	if (out_source == NULL) return false;
	if (controller_register_address == INTERRUPT_SOURCE_CONTROLLER_PLATFORM) {
		if (local_source_id >= 16u) return false;
		*out_source = (struct hal_interrupt_source){.domain = 0u, .number = local_source_id};
		return true;
	}
	if (controller_register_address != 0x10000000u || local_source_id >= 256u) return false;
	*out_source = (struct hal_interrupt_source){.domain = 0u, .number = local_source_id};
	return true;
}

bool hal_interrupt_source_claimable(const struct hal_interrupt_source* source) {
	return source != NULL && source->domain == 0u && source->number != 255u;
}

bool hal_interrupt_source_configuration_supported(const struct hal_interrupt_source* source,
                                                  enum hal_interrupt_trigger         trigger,
                                                  enum hal_interrupt_polarity        polarity) {
	return source != NULL && trigger <= HAL_INTERRUPT_TRIGGER_LEVEL && polarity <= HAL_INTERRUPT_POLARITY_LOW;
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
	last_source_target   = delivery->target;
	last_source_trigger  = delivery->trigger;
	last_source_polarity = delivery->polarity;
	return true;
}

bool hal_interrupt_source_mask(struct hal_interrupt_source_state* state) {
	if (state == NULL || !state->initialized || source_mask_fails) return false;
	state->masked = true;
	return true;
}

bool hal_interrupt_source_unmask(struct hal_interrupt_source_state* state) {
	if (state == NULL || !state->initialized || source_unmask_fails) return false;
	last_source_unmasked_from_masked = state->masked;
	state->masked                    = false;
	source_unmask_count++;
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
	return message_range_count;
}

bool hal_interrupt_message_range_at(size_t index, struct hal_interrupt_message_range* out_range) {
	if (index >= message_range_count || out_range == NULL) return false;
	*out_range = message_ranges[index];
	return true;
}

bool hal_interrupt_message_resolve(uint64_t controller_register_address, uint32_t producer_id,
                                   struct hal_interrupt_message_context* out_context) {
	if (controller_register_address != UINT64_MAX || producer_id != UINT32_MAX || out_context == NULL ||
	    message_range_count == 0u)
		return false;
	*out_context = (struct hal_interrupt_message_context){.domain = message_ranges[0].domain};
	return true;
}

bool hal_interrupt_message_target_supported(uint32_t domain, const struct hal_interrupt_message_source* source,
                                            const struct cpu* target) {
	return domain == 0u && target != NULL && (source == NULL || (message_source_supported && source->domain == domain));
}

bool hal_interrupt_message_init(struct hal_interrupt_message_state*         state,
                                const struct hal_interrupt_message_request* request,
                                struct hal_interrupt_message*               out_message) {
	if (state == NULL || state->initialized || request == NULL || out_message == NULL ||
	    !hal_interrupt_message_target_supported(request->domain, request->source, request->target) ||
	    request->event.domain != 0u)
		return false;
	bool in_range = false;
	for (size_t index = 0u; index < message_range_count; index++) {
		struct hal_interrupt_delivery_range range = message_ranges[index].delivery;
		if (message_ranges[index].domain == request->domain && request->event.domain == range.domain &&
		    request->event.id >= range.base && request->event.id < range.limit) {
			in_range = true;
			break;
		}
	}
	if (!in_range) return false;
	last_message_had_source = request->source != NULL;
	if (request->source != NULL) last_message_source = *request->source;
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
