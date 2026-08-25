#include <core/interrupt.h>
#include <core/signal.h>
#include <criterion/criterion.h>

#include "test_support.h"

#define INTERRUPT_TEST_OWNER ((process_id_t)42u)
#define INTERRUPT_TEST_OTHER ((process_id_t)84u)

Test(interrupt, exclusive_binding_masks_until_signal_rearm_and_publishes_kernel_signal) {
	struct signal*        signal;
	struct signal_message message;
	const interrupt_id_t  id = 7u;
	uint64_t              generation;

	ipc_test_init_heap();
	signal = signal_create();
	cr_assert_not_null(signal);
	cr_assert_eq(interrupt_attach(INTERRUPT_TEST_OWNER, id, signal), INTERRUPT_OK);
	cr_assert_eq(interrupt_attach(INTERRUPT_TEST_OTHER, id, signal), INTERRUPT_ALREADY_ATTACHED);
	cr_assert_eq(interrupt_attach(INTERRUPT_TEST_OWNER, id + 1u, signal), INTERRUPT_ALREADY_ATTACHED);

	cr_assert(interrupt_dispatch(id));
	cr_assert_eq(signal_read(signal, &message), SIGNAL_OK);
	cr_assert_eq(message.sender, SIGNAL_SENDER_KERNEL);
	cr_assert_eq(message.payload.args[0], id);
	generation = signal_generation(signal);
	cr_assert_neq(generation, 0u);

	cr_assert(interrupt_dispatch(id));
	cr_assert_eq(signal_generation(signal), generation, "masked interrupt published twice before re-arm");
	cr_assert(interrupt_rearm_signal(signal_id(signal)));
	cr_assert(interrupt_dispatch(id));
	cr_assert_neq(signal_generation(signal), generation);

	cr_assert_eq(interrupt_detach(INTERRUPT_TEST_OTHER, id), INTERRUPT_NOT_OWNER);
	cr_assert_eq(interrupt_detach(INTERRUPT_TEST_OWNER, id), INTERRUPT_OK);
	cr_assert_not(interrupt_dispatch(id));
	cr_assert_eq(signal_destroy(signal), SIGNAL_OK);
}

Test(interrupt, process_cleanup_detaches_all_owned_sources) {
	struct signal*       first;
	struct signal*       second;
	const interrupt_id_t first_id  = 8u;
	const interrupt_id_t second_id = 9u;

	ipc_test_init_heap();
	first  = signal_create();
	second = signal_create();
	cr_assert_not_null(first);
	cr_assert_not_null(second);
	cr_assert_eq(interrupt_attach(INTERRUPT_TEST_OWNER, first_id, first), INTERRUPT_OK);
	cr_assert_eq(interrupt_attach(INTERRUPT_TEST_OWNER, second_id, second), INTERRUPT_OK);

	interrupt_cleanup_process(INTERRUPT_TEST_OTHER);
	cr_assert(interrupt_dispatch(first_id));
	cr_assert(interrupt_dispatch(second_id));
	cr_assert(interrupt_rearm_signal(signal_id(first)));
	cr_assert(interrupt_rearm_signal(signal_id(second)));

	interrupt_cleanup_process(INTERRUPT_TEST_OWNER);
	cr_assert_not(interrupt_dispatch(first_id));
	cr_assert_not(interrupt_dispatch(second_id));
	cr_assert_eq(signal_destroy(first), SIGNAL_OK);
	cr_assert_eq(signal_destroy(second), SIGNAL_OK);
}
