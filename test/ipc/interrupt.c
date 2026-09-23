#include <core/address_space.h>
#include <core/cpu.h>
#include <core/interrupt.h>
#include <core/signal.h>
#include <core/user_upcall.h>
#include <core/uthread.h>
#include <criterion/criterion.h>
#include <hal/interrupts.h>
#include <libc/string.h>

#include "test_support.h"

static struct cpu interrupt_test_cpu = {.index = 0u, .role = CPU_ROLE_BSP};

extern const struct cpu*           hal_interrupt_mock_last_target(void);
extern bool                        hal_interrupt_mock_unmasked_from_masked(void);
extern size_t                      hal_interrupt_mock_unmask_count(void);
extern void                        hal_interrupt_mock_set_mask_failure(bool fail);
extern void                        hal_interrupt_mock_set_unmask_failure(bool fail);
extern enum hal_interrupt_trigger  hal_interrupt_mock_last_trigger(void);
extern enum hal_interrupt_polarity hal_interrupt_mock_last_polarity(void);
extern void hal_interrupt_mock_set_message_ranges(const struct hal_interrupt_message_range* ranges, size_t count);
extern void hal_interrupt_mock_set_message_source_supported(bool supported);
extern bool hal_interrupt_mock_last_message_source(struct hal_interrupt_message_source* out_source);

struct hal_userspace_return_frame {
	bool      user;
	uintptr_t entry;
	uintptr_t stack;
	uintptr_t args[USER_UPCALL_ARGUMENT_COUNT];
};

static void interrupt_test_handler(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4) {
	(void)arg0;
	(void)arg1;
	(void)arg2;
	(void)arg3;
	(void)arg4;
}

static void interrupt_test_init_uthread(struct uthread* target, uintptr_t stack_top) {
	ipc_test_init_heap();
	memset(target, 0, sizeof(*target));
	target->process         = (struct process*)(uintptr_t)1u;
	target->reference_count = 1u;
	cr_assert(uthread_upcall_state_init(target));
	target->upcall.stack_mapping = (struct mapping*)1u;
	target->upcall.stack_top     = stack_top;
}

static struct hal_userspace_return_frame interrupt_test_frame(void) {
	return (struct hal_userspace_return_frame){.user = true, .entry = 0x1000u, .stack = 0x8000u};
}

static struct interrupt* interrupt_test_bind_source(struct signal* signal, uint32_t source_number) {
	struct hal_interrupt_source source = {.domain = 0u, .number = source_number};
	interrupt_source_t          token;
	struct interrupt*           interrupt;

	cr_assert(interrupt_register_source(&source, HAL_INTERRUPT_TRIGGER_EDGE, HAL_INTERRUPT_POLARITY_HIGH, &token) ==
	          INTERRUPT_OK);
	cr_assert(interrupt_claim_source(token, &interrupt) == INTERRUPT_OK);
	cr_assert(interrupt_bind(interrupt, signal) == INTERRUPT_OK);
	return interrupt;
}

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
	struct interrupt*           interrupts[3] = {0};
	struct interrupt_message    messages[3];
	interrupt_message_context_t context;

	cpu_bind_current(&interrupt_test_cpu);
	ipc_test_init_heap();
	cr_assert(interrupt_init());
	cr_assert(interrupt_message_context_create(0u, NULL, &context));
	hal_interrupt_mock_set_message_ranges(ranges, sizeof(ranges) / sizeof(ranges[0]));
	for (size_t index = 0u; index < 3u; index++) {
		enum interrupt_result result = interrupt_allocate_message(context, &interrupts[index], &messages[index]);
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
	cr_assert_eq(interrupt_allocate_message(context, &unused_interrupt, &unused_message), INTERRUPT_NOT_FOUND);
	const struct hal_interrupt_message_range default_range = {
		.domain = 0u, .delivery = {.domain = 0u, .base = 0u, .limit = 256u}
    };
	hal_interrupt_mock_set_message_ranges(&default_range, 1u);

	const struct hal_interrupt_message_source producer = {.domain = 0u, .id = 37u};
	struct hal_interrupt_message_source       decoded;
	hal_interrupt_mock_set_message_source_supported(true);
	cr_assert(interrupt_message_context_create(0u, &producer, &context));
	cr_assert_eq(interrupt_allocate_message(context, &unused_interrupt, &unused_message), INTERRUPT_OK);
	cr_assert(hal_interrupt_mock_last_message_source(&decoded));
	cr_assert_eq(decoded.domain, producer.domain);
	cr_assert_eq(decoded.id, producer.id);
	cr_assert_eq(interrupt_destroy(unused_interrupt), INTERRUPT_OK);
	interrupt_release(unused_interrupt);
	hal_interrupt_mock_set_message_source_supported(false);
	cr_assert_not(interrupt_message_context_create(1u, &producer, &context));
	cr_assert_not(interrupt_message_context_create(UINT32_MAX, NULL, &context));
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

Test(interrupt_core, interrupt_upcalls_form_a_completion_barrier) {
	struct signal*                    signal;
	struct interrupt*                 interrupt;
	struct uthread                    persistent;
	struct uthread                    oneshot;
	struct hal_userspace_return_frame persistent_frame = interrupt_test_frame();
	struct hal_userspace_return_frame oneshot_frame    = interrupt_test_frame();
	size_t                            unmask_count;

	cpu_bind_current(&interrupt_test_cpu);
	ipc_test_init_heap();
	cr_assert(interrupt_init());
	signal = signal_create();
	cr_assert_not_null(signal);
	interrupt_test_init_uthread(&persistent, 0x9000u);
	interrupt_test_init_uthread(&oneshot, 0xa000u);
	cr_assert_eq(signal_register_handler(signal, &persistent, interrupt_test_handler, SIGNAL_HANDLER_FLAG_NONE),
	             SIGNAL_OK);
	cr_assert_eq(signal_register_handler(signal, &oneshot, interrupt_test_handler, SIGNAL_HANDLER_FLAG_ONESHOT),
	             SIGNAL_OK);
	interrupt    = interrupt_test_bind_source(signal, 11u);
	unmask_count = hal_interrupt_mock_unmask_count();

	cr_assert(interrupt_handle_event((struct hal_interrupt_event){.domain = 0u, .id = 11u}));
	cr_assert_eq(persistent.upcall.pending[persistent.upcall.head].origin, USER_UPCALL_ORIGIN_INTERRUPT_SIGNAL);
	cr_assert((persistent.upcall.pending[persistent.upcall.head].flags & USER_UPCALL_FLAG_INTERRUPT_REARM) != 0u);
	cr_assert_eq(oneshot.upcall.pending[oneshot.upcall.head].origin, USER_UPCALL_ORIGIN_INTERRUPT_SIGNAL);
	cr_assert((oneshot.upcall.pending[oneshot.upcall.head].flags & USER_UPCALL_FLAG_INTERRUPT_REARM) == 0u);

	cr_assert_eq(uthread_upcall_deliver(&persistent, &persistent_frame), USER_UPCALL_OK);
	cr_assert_eq(uthread_upcall_restore(&persistent, &persistent_frame), USER_UPCALL_OK);
	cr_assert_eq(hal_interrupt_mock_unmask_count(), unmask_count);
	cr_assert_eq(uthread_upcall_deliver(&oneshot, &oneshot_frame), USER_UPCALL_OK);
	cr_assert_eq(uthread_upcall_restore(&oneshot, &oneshot_frame), USER_UPCALL_OK);
	cr_assert_eq(hal_interrupt_mock_unmask_count(), unmask_count + 1u);

	cr_assert_eq(signal_destroy(signal), SIGNAL_OK);
	cr_assert_eq(interrupt_destroy(interrupt), INTERRUPT_OK);
	interrupt_release(interrupt);
	cr_assert_eq(__atomic_load_n(&persistent.reference_count, __ATOMIC_ACQUIRE), 1u);
	cr_assert_eq(__atomic_load_n(&oneshot.reference_count, __ATOMIC_ACQUIRE), 1u);
	uthread_upcall_state_deinit(&persistent);
	uthread_upcall_state_deinit(&oneshot);
}

Test(interrupt_core, oneshot_and_ordinary_upcalls_do_not_request_rearm) {
	struct signal*                    signal;
	struct interrupt*                 interrupt;
	struct uthread                    ordinary;
	struct uthread                    oneshot;
	struct hal_userspace_return_frame ordinary_frame = interrupt_test_frame();
	struct hal_userspace_return_frame oneshot_frame  = interrupt_test_frame();
	struct signal_payload             payload        = {0};
	size_t                            unmask_count;

	cpu_bind_current(&interrupt_test_cpu);
	ipc_test_init_heap();
	cr_assert(interrupt_init());
	signal = signal_create();
	cr_assert_not_null(signal);
	interrupt_test_init_uthread(&ordinary, 0x9000u);
	interrupt_test_init_uthread(&oneshot, 0xa000u);
	cr_assert_eq(signal_register_handler(signal, &ordinary, interrupt_test_handler, SIGNAL_HANDLER_FLAG_NONE),
	             SIGNAL_OK);
	interrupt    = interrupt_test_bind_source(signal, 12u);
	unmask_count = hal_interrupt_mock_unmask_count();

	cr_assert_eq(signal_send(signal, 42u, &payload, NULL, NULL), SIGNAL_OK);
	cr_assert_eq(ordinary.upcall.pending[ordinary.upcall.head].origin, USER_UPCALL_ORIGIN_SIGNAL);
	cr_assert_eq(uthread_upcall_deliver(&ordinary, &ordinary_frame), USER_UPCALL_OK);
	cr_assert_eq(uthread_upcall_restore(&ordinary, &ordinary_frame), USER_UPCALL_OK);
	cr_assert_eq(hal_interrupt_mock_unmask_count(), unmask_count);
	cr_assert_eq(signal_unregister_handler(signal, &ordinary), SIGNAL_OK);

	cr_assert_eq(signal_register_handler(signal, &oneshot, interrupt_test_handler, SIGNAL_HANDLER_FLAG_ONESHOT),
	             SIGNAL_OK);
	cr_assert(interrupt_handle_event((struct hal_interrupt_event){.domain = 0u, .id = 12u}));
	cr_assert_eq(uthread_upcall_deliver(&oneshot, &oneshot_frame), USER_UPCALL_OK);
	cr_assert_eq(uthread_upcall_restore(&oneshot, &oneshot_frame), USER_UPCALL_OK);
	cr_assert_eq(hal_interrupt_mock_unmask_count(), unmask_count);

	cr_assert_eq(signal_destroy(signal), SIGNAL_OK);
	cr_assert_eq(interrupt_destroy(interrupt), INTERRUPT_OK);
	interrupt_release(interrupt);
	cr_assert_eq(__atomic_load_n(&ordinary.reference_count, __ATOMIC_ACQUIRE), 1u);
	cr_assert_eq(__atomic_load_n(&oneshot.reference_count, __ATOMIC_ACQUIRE), 1u);
	uthread_upcall_state_deinit(&ordinary);
	uthread_upcall_state_deinit(&oneshot);
}

Test(interrupt_core, registering_handler_rearms_after_oneshot_completion) {
	struct signal*                    signal;
	struct interrupt*                 interrupt;
	struct uthread                    oneshot;
	struct hal_userspace_return_frame frame = interrupt_test_frame();
	size_t                            unmask_count;

	cpu_bind_current(&interrupt_test_cpu);
	ipc_test_init_heap();
	cr_assert(interrupt_init());
	signal = signal_create();
	cr_assert_not_null(signal);
	interrupt_test_init_uthread(&oneshot, 0x9000u);
	cr_assert_eq(signal_register_handler(signal, &oneshot, interrupt_test_handler, SIGNAL_HANDLER_FLAG_ONESHOT),
	             SIGNAL_OK);
	interrupt    = interrupt_test_bind_source(signal, 13u);
	unmask_count = hal_interrupt_mock_unmask_count();

	cr_assert(interrupt_handle_event((struct hal_interrupt_event){.domain = 0u, .id = 13u}));
	cr_assert_eq(uthread_upcall_deliver(&oneshot, &frame), USER_UPCALL_OK);
	cr_assert_eq(uthread_upcall_restore(&oneshot, &frame), USER_UPCALL_OK);
	cr_assert_eq(hal_interrupt_mock_unmask_count(), unmask_count);

	cr_assert_eq(signal_register_handler(signal, &oneshot, interrupt_test_handler, SIGNAL_HANDLER_FLAG_ONESHOT),
	             SIGNAL_OK);
	cr_assert_eq(hal_interrupt_mock_unmask_count(), unmask_count + 1u);
	cr_assert(interrupt_handle_event((struct hal_interrupt_event){.domain = 0u, .id = 13u}));
	cr_assert_eq(oneshot.upcall.count, 1u);
	cr_assert_eq(oneshot.upcall.pending[oneshot.upcall.head].origin, USER_UPCALL_ORIGIN_INTERRUPT_SIGNAL);
	cr_assert_eq(uthread_upcall_deliver(&oneshot, &frame), USER_UPCALL_DEFERRED);
	cr_assert_eq(uthread_upcall_deliver(&oneshot, &frame), USER_UPCALL_OK);
	cr_assert_eq(uthread_upcall_restore(&oneshot, &frame), USER_UPCALL_OK);

	cr_assert_eq(signal_destroy(signal), SIGNAL_OK);
	cr_assert_eq(interrupt_destroy(interrupt), INTERRUPT_OK);
	interrupt_release(interrupt);
	cr_assert_eq(__atomic_load_n(&oneshot.reference_count, __ATOMIC_ACQUIRE), 1u);
	uthread_upcall_state_deinit(&oneshot);
}

Test(interrupt_core, signal_refusal_is_not_reported_as_already_bound) {
	struct signal*    signal;
	struct interrupt* interrupt;
	struct uthread    target;

	cpu_bind_current(&interrupt_test_cpu);
	ipc_test_init_heap();
	cr_assert(interrupt_init());
	signal = signal_create();
	cr_assert_not_null(signal);
	interrupt_test_init_uthread(&target, 0x9000u);
	cr_assert_eq(signal_register_handler(signal, &target, interrupt_test_handler, SIGNAL_HANDLER_FLAG_NONE), SIGNAL_OK);
	interrupt = interrupt_test_bind_source(signal, 14u);

	cr_assert(interrupt_handle_event((struct hal_interrupt_event){.domain = 0u, .id = 14u}));
	cr_assert_eq(interrupt_unbind(interrupt), INTERRUPT_OK);
	cr_assert_eq(interrupt_bind(interrupt, signal), INTERRUPT_UNAVAILABLE);
	cr_assert_eq(signal_unregister_handler(signal, &target), SIGNAL_OK);
	cr_assert_eq(interrupt_bind(interrupt, signal), INTERRUPT_OK);

	cr_assert_eq(signal_destroy(signal), SIGNAL_OK);
	cr_assert_eq(interrupt_destroy(interrupt), INTERRUPT_OK);
	interrupt_release(interrupt);
	cr_assert_eq(__atomic_load_n(&target.reference_count, __ATOMIC_ACQUIRE), 1u);
	uthread_upcall_state_deinit(&target);
}

Test(interrupt_core, routing_table_retains_published_interrupts) {
	const struct hal_interrupt_source source = {.domain = 0u, .number = 15u};
	interrupt_message_context_t       context;
	struct interrupt_message          message;
	interrupt_source_t                token;
	struct interrupt*                 source_interrupt;
	struct interrupt*                 message_interrupt;
	struct interrupt_info             info;

	cpu_bind_current(&interrupt_test_cpu);
	ipc_test_init_heap();
	cr_assert(interrupt_init());
	cr_assert_eq(interrupt_register_source(&source, HAL_INTERRUPT_TRIGGER_EDGE, HAL_INTERRUPT_POLARITY_HIGH, &token),
	             INTERRUPT_OK);
	cr_assert_eq(interrupt_claim_source(token, &source_interrupt), INTERRUPT_OK);
	interrupt_release(source_interrupt);
	cr_assert(interrupt_retain(source_interrupt));
	cr_assert_eq(interrupt_get_info(source_interrupt, &info), INTERRUPT_OK);
	cr_assert_eq(interrupt_destroy(source_interrupt), INTERRUPT_OK);
	interrupt_release(source_interrupt);

	cr_assert(interrupt_message_context_create(0u, NULL, &context));
	cr_assert_eq(interrupt_allocate_message(context, &message_interrupt, &message), INTERRUPT_OK);
	interrupt_release(message_interrupt);
	cr_assert(interrupt_retain(message_interrupt));
	cr_assert_eq(interrupt_get_info(message_interrupt, &info), INTERRUPT_OK);
	cr_assert_eq(interrupt_destroy(message_interrupt), INTERRUPT_OK);
	interrupt_release(message_interrupt);
}
