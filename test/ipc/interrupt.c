#include <core/cpu.h>
#include <core/interrupt.h>
#include <core/signal.h>
#include <criterion/criterion.h>
#include <hal/interrupts.h>

#include "test_support.h"

static struct cpu interrupt_test_cpu = {.index = 0u, .role = CPU_ROLE_BSP};

extern const struct cpu*           hal_interrupt_mock_last_target(void);
extern bool                        hal_interrupt_mock_unmasked_from_masked(void);
extern void                        hal_interrupt_mock_set_mask_failure(bool fail);
extern void                        hal_interrupt_mock_set_unmask_failure(bool fail);
extern enum hal_interrupt_trigger  hal_interrupt_mock_last_trigger(void);
extern enum hal_interrupt_polarity hal_interrupt_mock_last_polarity(void);
extern void hal_interrupt_mock_set_message_ranges(const struct hal_interrupt_message_range* ranges, size_t count);

Test(interrupt_hal, fixed_sources_start_masked_and_messages_compose) {
	struct hal_interrupt_source             source;
	struct hal_interrupt_source_state       fixed = {0};
	struct hal_interrupt_delivery           delivery;
	struct hal_interrupt_message_range      range;
	struct hal_interrupt_message            message;
	struct hal_interrupt_source_domain_info fixed_domain;

	cpu_bind_current(&interrupt_test_cpu);
	cr_assert_eq(hal_interrupt_source_domain_count(), 1u);
	cr_assert(hal_interrupt_source_domain_at(0u, &fixed_domain));
	cr_assert_eq(fixed_domain.domain, 0u);
	cr_assert_eq(fixed_domain.source_count, 256u);
	source = (struct hal_interrupt_source){.domain = fixed_domain.domain, .number = 7u};
	struct hal_interrupt_source_info source_info;
	cr_assert(hal_interrupt_source_info(&source, &source_info));
	cr_assert_eq(source_info.delivery.base, source.number);
	cr_assert_eq(source_info.delivery.limit, source.number + 1u);
	cr_assert_eq(source_info.target_kind, HAL_INTERRUPT_TARGET_ROUTABLE);
	cr_assert_null(source_info.fixed_target);
	cr_assert(hal_interrupt_source_target_supported(&source, &interrupt_test_cpu));
	cr_assert_not(hal_interrupt_source_target_supported(&source, NULL));
	delivery = (struct hal_interrupt_delivery){
		.target   = &interrupt_test_cpu,
		.event    = {.domain = source_info.delivery.domain, .id = source_info.delivery.base},
		.trigger  = HAL_INTERRUPT_TRIGGER_FIRMWARE,
		.polarity = HAL_INTERRUPT_POLARITY_FIRMWARE
    };
	cr_assert_not(hal_interrupt_source_mask(&fixed));
	cr_assert_not(hal_interrupt_source_unmask(&fixed));
	cr_assert(hal_interrupt_source_init(&fixed, &source, &delivery));
	cr_assert(fixed.initialized && fixed.masked);
	cr_assert_not(hal_interrupt_source_init(&fixed, &source, &delivery));
	cr_assert(fixed.initialized && fixed.masked);
	cr_assert_eq(fixed.target, &interrupt_test_cpu);
	cr_assert_eq(hal_interrupt_mock_last_target(), &interrupt_test_cpu);
	cr_assert(hal_interrupt_source_unmask(&fixed));
	cr_assert(hal_interrupt_mock_unmasked_from_masked());
	cr_assert_not(fixed.masked);
	cr_assert(hal_interrupt_source_mask(&fixed));
	cr_assert(fixed.masked);
	cr_assert(hal_interrupt_source_unmask(&fixed));
	hal_interrupt_mock_set_mask_failure(true);
	cr_assert_not(hal_interrupt_source_deinit(&fixed));
	cr_assert(fixed.initialized && !fixed.masked);
	hal_interrupt_mock_set_mask_failure(false);
	cr_assert(hal_interrupt_source_deinit(&fixed));
	cr_assert_not(fixed.initialized);
	cr_assert_not(hal_interrupt_source_mask(&fixed));
	cr_assert_not(hal_interrupt_source_unmask(&fixed));
	struct hal_interrupt_source invalid_source = {.domain = fixed_domain.domain + 1u, .number = source.number};
	cr_assert_not(hal_interrupt_source_init(&fixed, &invalid_source, &delivery));
	cr_assert_not(fixed.initialized);
	delivery.event.id = fixed_domain.source_count;
	cr_assert_not(hal_interrupt_source_init(&fixed, &source, &delivery));
	cr_assert_not(fixed.initialized);
	delivery.event.id = source.number;
	cr_assert_eq(hal_interrupt_message_range_count(), 1u);
	cr_assert(hal_interrupt_message_range_at(0u, &range));
	cr_assert(range.delivery.base <= 64u && range.delivery.limit > 64u);
	struct hal_interrupt_message_request request = {
		.domain = range.domain, .target = &interrupt_test_cpu, .event = {.domain = range.delivery.domain, .id = 64u}
    };
	cr_assert(hal_interrupt_message_target_supported(request.domain, request.source, request.target));
	cr_assert_not(hal_interrupt_message_target_supported(request.domain, request.source, NULL));
	struct hal_interrupt_message_state message_state = {0};
	cr_assert(hal_interrupt_message_init(&message_state, &request, &message));
	cr_assert_eq(message.address, 0xfee00000u);
	cr_assert_eq(message.data, 64u);
	cr_assert_not(hal_interrupt_message_init(&message_state, &request, &message));
	cr_assert(hal_interrupt_message_deinit(&message_state));
	struct hal_interrupt_message_source producer = {.domain = 0u, .id = 1u};
	request.source                               = &producer;
	cr_assert_not(hal_interrupt_message_target_supported(request.domain, request.source, request.target));
	cr_assert_not(hal_interrupt_message_init(&message_state, &request, &message));
	request.source = NULL;
	request.domain++;
	cr_assert_not(hal_interrupt_message_init(&message_state, &request, &message));
	cr_assert(hal_interrupt_message_deinit(&message_state));
}

Test(interrupt_core, message_allocation_tries_every_matching_range) {
	const struct hal_interrupt_message_range ranges[] = {
		{.domain = 0u, .delivery = {.domain = 0u, .base = 10u, .limit = 12u}},
		{.domain = 1u,   .delivery = {.domain = 1u, .base = 0u, .limit = 1u}},
		{.domain = 0u, .delivery = {.domain = 0u, .base = 20u, .limit = 22u}},
	};
	struct interrupt*        interrupts[3] = {0};
	struct interrupt_message messages[3];

	cpu_bind_current(&interrupt_test_cpu);
	ipc_test_init_heap();
	cr_assert(interrupt_init());
	hal_interrupt_mock_set_message_ranges(ranges, sizeof(ranges) / sizeof(ranges[0]));
	for (size_t index = 0u; index < 3u; index++) {
		enum interrupt_result result = interrupt_allocate_message(0u, &interrupts[index], &messages[index]);
		cr_assert_eq(result, INTERRUPT_OK, "allocation %zu failed with %d", index, (int)result);
	}
	cr_assert_eq(messages[0].message_data, 10u);
	cr_assert_eq(messages[1].message_data, 11u);
	cr_assert_eq(messages[2].message_data, 20u);
	for (size_t index = 0u; index < 3u; index++) {
		cr_assert_eq(interrupt_destroy(interrupts[index]), INTERRUPT_OK);
		interrupt_release(interrupts[index]);
	}

	hal_interrupt_mock_set_message_ranges(NULL, 0u);
	struct interrupt*        unused_interrupt = NULL;
	struct interrupt_message unused_message;
	cr_assert_eq(interrupt_allocate_message(0u, &unused_interrupt, &unused_message), INTERRUPT_NOT_FOUND);
	const struct hal_interrupt_message_range default_range = {
		.domain = 0u, .delivery = {.domain = 0u, .base = 0u, .limit = 256u}
    };
	hal_interrupt_mock_set_message_ranges(&default_range, 1u);
}

Test(interrupt_core, opaque_source_configuration_and_signal_lifecycle) {
	const struct hal_interrupt_source source = {.domain = 0u, .number = 7u};
	interrupt_source_t                token;
	struct interrupt*                 interrupt;
	struct interrupt_info             info;
	struct signal*                    signal;

	cpu_bind_current(&interrupt_test_cpu);
	ipc_test_init_heap();
	cr_assert(interrupt_init());
	enum interrupt_result register_result =
		interrupt_register_source(&source, HAL_INTERRUPT_TRIGGER_EDGE, HAL_INTERRUPT_POLARITY_LOW, &token);
	cr_assert_eq(register_result, INTERRUPT_OK, "registration failed with %d", (int)register_result);
	cr_assert_neq(token, INTERRUPT_SOURCE_INVALID);
	cr_assert_eq(interrupt_claim_source(token, &interrupt), INTERRUPT_OK);
	cr_assert_eq(hal_interrupt_mock_last_trigger(), HAL_INTERRUPT_TRIGGER_EDGE);
	cr_assert_eq(hal_interrupt_mock_last_polarity(), HAL_INTERRUPT_POLARITY_LOW);

	signal = signal_create();
	cr_assert_not_null(signal);
	cr_assert_eq(interrupt_bind(interrupt, signal), INTERRUPT_OK);
	cr_assert(interrupt_handle_event((struct hal_interrupt_event){.domain = 0u, .id = 7u}));
	cr_assert(signal_has_value(signal));

	hal_interrupt_mock_set_unmask_failure(true);
	interrupt_signal_ready(signal_id(signal));
	hal_interrupt_mock_set_unmask_failure(false);
	interrupt_signal_ready(signal_id(signal));
	cr_assert(hal_interrupt_mock_unmasked_from_masked());

	hal_interrupt_mock_set_mask_failure(true);
	cr_assert_eq(signal_destroy(signal), SIGNAL_UNAVAILABLE);
	cr_assert_eq(interrupt_get_info(interrupt, &info), INTERRUPT_OK);
	cr_assert(info.bound);
	hal_interrupt_mock_set_mask_failure(false);
	cr_assert_eq(signal_destroy(signal), SIGNAL_OK);
	cr_assert_eq(interrupt_get_info(interrupt, &info), INTERRUPT_OK);
	cr_assert_not(info.bound);
	cr_assert_eq(interrupt_destroy(interrupt), INTERRUPT_OK);
	interrupt_release(interrupt);
}
