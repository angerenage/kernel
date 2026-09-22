#include <core/cpu.h>
#include <criterion/criterion.h>
#include <hal/interrupts.h>

static struct cpu interrupt_test_cpu = {.index = 0u, .role = CPU_ROLE_BSP};

extern const struct cpu* hal_interrupt_mock_last_target(void);
extern bool              hal_interrupt_mock_unmasked_from_masked(void);
extern void              hal_interrupt_mock_set_mask_failure(bool fail);

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
