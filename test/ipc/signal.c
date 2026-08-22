#include "../../kernel/src/capability/signal.h"

#include <core/capability.h>
#include <core/cpu.h>
#include <core/sched.h>
#include <core/signal.h>
#include <core/thread.h>
#include <core/user_upcall.h>
#include <core/uthread.h>
#include <criterion/criterion.h>
#include <hal/cpu.h>
#include <hal/interrupts.h>
#include <libc/string.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../mocks/hal/cpu_mock.h"
#include "test_support.h"

#define SIGNAL_TEST_SENDER ((process_id_t)42u)
#define SIGNAL_TEST_SECOND_SENDER ((process_id_t)84u)

static bool                  signal_test_hook_active;
static size_t                signal_test_hook_runs;
static struct signal*        signal_test_signal;
static struct thread*        signal_test_sender;
static struct signal_payload signal_test_first_payload;
static struct signal_payload signal_test_second_payload;
static bool                  signal_test_send_twice;
static bool                  signal_test_force_retry;
static struct uthread*       signal_test_force_blocked_handler;
static uintptr_t             signal_test_force_purge_token;
static uint64_t              signal_test_first_receivers;
static uint64_t              signal_test_first_deliveries;
static uint64_t              signal_test_second_receivers;
static uint64_t              signal_test_second_deliveries;

static void signal_test_handler(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4) {
	(void)arg0;
	(void)arg1;
	(void)arg2;
	(void)arg3;
	(void)arg4;
}

static void signal_test_replacement_handler(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
                                            uintptr_t arg4) {
	(void)arg0;
	(void)arg1;
	(void)arg2;
	(void)arg3;
	(void)arg4;
}

static void signal_test_thread_entry(void* arg) {
	(void)arg;
}

static void signal_test_init_bound_bootstrap_cpu(void) {
	irq_enable_local();
	cr_assert(cpu_topology_init_bootstrap(0x100000u, 0x104000u), "cpu_topology_init_bootstrap failed");
	cr_assert_not_null(cpu_bsp(), "cpu_bsp returned NULL");
	cpu_bind_current(cpu_bsp());
	cpu_interrupts_set_ready(cpu_current(), false);
	cr_assert(sched_init(), "sched_init failed");
	cr_assert(sched_start_cpu(cpu_current()), "sched_start_cpu failed");
}

static void signal_test_reset_scheduler_state(void) {
	irq_enable_local();
	hal_cpu_mock_set_context_switch_hook(NULL);
	signal_test_hook_active           = false;
	signal_test_hook_runs             = 0u;
	signal_test_signal                = NULL;
	signal_test_sender                = NULL;
	signal_test_first_payload         = (struct signal_payload){0};
	signal_test_second_payload        = (struct signal_payload){0};
	signal_test_send_twice            = false;
	signal_test_force_retry           = false;
	signal_test_force_blocked_handler = NULL;
	signal_test_force_purge_token     = 0u;
	signal_test_first_receivers       = 0u;
	signal_test_first_deliveries      = 0u;
	signal_test_second_receivers      = 0u;
	signal_test_second_deliveries     = 0u;
	hal_cpu_local_bind(NULL);
}

static void signal_test_init_sched_uthread(struct uthread* target, const char* name, uintptr_t stack_base,
                                           uintptr_t stack_top, uintptr_t upcall_stack_top) {
	struct thread_create_params params = {
		.name              = name,
		.entry             = signal_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = stack_base,
		.kernel_stack_top  = stack_top,
		.preferred_cpu     = NULL,
		.detached          = false,
	};

	ipc_test_init_heap();
	memset(target, 0, sizeof(*target));
	cr_assert(thread_init(&target->thread, &params), "uthread scheduler descriptor initialization failed");
	target->thread.owner_kind = THREAD_OWNER_UTHREAD;
	target->thread.owner      = target;
	target->process           = (struct process*)(uintptr_t)1u;
	target->reference_count   = 1u;
	cr_assert(uthread_upcall_state_init(target), "uthread upcall state initialization failed");
	target->upcall.stack_id  = 1u;
	target->upcall.stack_top = upcall_stack_top;
}

static void signal_test_init_handler_uthread(struct uthread* target, uintptr_t upcall_stack_top) {
	ipc_test_init_heap();
	memset(target, 0, sizeof(*target));
	target->process         = (struct process*)(uintptr_t)1u;
	target->reference_count = 1u;
	cr_assert(uthread_upcall_state_init(target), "uthread upcall state initialization failed");
	target->upcall.stack_id  = 1u;
	target->upcall.stack_top = upcall_stack_top;
}

static void signal_test_deinit_uthread(struct uthread* target) {
	cr_assert_eq(__atomic_load_n(&target->reference_count, __ATOMIC_ACQUIRE),
	             1u,
	             "signal receivers must release their uthread references");
	uthread_upcall_state_deinit(target);
}

static void signal_test_send_context_switch_hook(struct thread_context* current, const struct thread_context* next) {
	(void)current;
	(void)next;

	if (signal_test_hook_active || signal_test_hook_runs != 0u) return;

	signal_test_hook_active = true;
	signal_test_hook_runs++;
	cr_assert_eq(sched_current_thread(), signal_test_sender, "sender should run while the receiver is blocked");
	if (signal_test_force_retry) {
		cr_assert_not_null(signal_test_force_blocked_handler);
		cr_assert_neq(signal_test_force_purge_token, 0u);
		cr_assert_eq(signal_send_force(signal_test_signal,
		                               SIGNAL_TEST_SENDER,
		                               &signal_test_first_payload,
		                               &signal_test_first_receivers,
		                               &signal_test_first_deliveries),
		             SIGNAL_UNAVAILABLE);
		cr_assert_eq(signal_test_first_receivers, 0u);
		cr_assert_eq(signal_test_first_deliveries, 0u);
		cr_assert_eq(signal_generation(signal_test_signal), 0u);
		cr_assert_eq(signal_blocked_waiter_count(signal_test_signal), 1u);
		cr_assert_eq(uthread_upcall_purge(
						 signal_test_force_blocked_handler, USER_UPCALL_ORIGIN_SIGNAL, signal_test_force_purge_token),
		             1u);
		cr_assert_eq(signal_send_force(signal_test_signal,
		                               SIGNAL_TEST_SECOND_SENDER,
		                               &signal_test_second_payload,
		                               &signal_test_second_receivers,
		                               &signal_test_second_deliveries),
		             SIGNAL_OK);
	}
	else {
		cr_assert_eq(signal_send(signal_test_signal,
		                         SIGNAL_TEST_SENDER,
		                         &signal_test_first_payload,
		                         &signal_test_first_receivers,
		                         &signal_test_first_deliveries),
		             SIGNAL_OK);
		if (signal_test_send_twice) {
			cr_assert_eq(signal_send(signal_test_signal,
			                         SIGNAL_TEST_SECOND_SENDER,
			                         &signal_test_second_payload,
			                         &signal_test_second_receivers,
			                         &signal_test_second_deliveries),
			             SIGNAL_OK);
		}
	}
	sched_yield();
	signal_test_hook_active = false;
}

Test(signal, create_send_read_and_destroy) {
	struct signal*        signal;
	struct signal*        acquired;
	struct signal_payload payload = {
		.args = {1u, 2u, 3u, 4u}
    };
	struct signal_message received;
	uint64_t              receiver_count = UINT64_MAX;
	uint64_t              delivery_count = UINT64_MAX;
	signal_id_t           id;

	ipc_test_init_heap();
	signal = signal_create();
	cr_assert_not_null(signal);
	id = signal_id(signal);
	cr_assert_neq(id, SIGNAL_ID_INVALID);
	cr_assert_eq(signal_count(), 1u);
	cr_assert_eq(signal_read(signal, &received), SIGNAL_NO_VALUE);

	acquired = signal_acquire(id);
	cr_assert_eq(acquired, signal);
	cr_assert_eq(signal_send(signal, SIGNAL_SENDER_KERNEL, &payload, &receiver_count, &delivery_count), SIGNAL_OK);
	cr_assert_eq(receiver_count, 0u);
	cr_assert_eq(delivery_count, 0u);
	cr_assert(signal_has_value(signal));
	cr_assert_eq(signal_generation(signal), 1u);
	cr_assert_eq(signal_read(signal, &received), SIGNAL_OK);
	cr_assert_eq(received.sender, SIGNAL_SENDER_KERNEL);
	cr_assert_eq(memcmp(&payload, &received.payload, sizeof(payload)), 0);

	cr_assert_eq(signal_destroy(signal), SIGNAL_OK);
	cr_assert_null(signal_acquire(id));
	cr_assert_eq(signal_count(), 0u);
	cr_assert_eq(signal_read(acquired, &received), SIGNAL_CLOSED);
	signal_release(acquired);
}

Test(signal, failed_destroy_restores_open_state) {
	struct signal* signal;
	struct signal* acquired;
	signal_id_t    id;

	ipc_test_init_heap();
	signal = signal_create();
	cr_assert_not_null(signal);
	id = signal_id(signal);
	cr_assert_neq(id, SIGNAL_ID_INVALID);

	signal->id = SIGNAL_ID_INVALID;
	cr_assert_eq(signal_destroy(signal), SIGNAL_NOT_FOUND);
	cr_assert_not(signal->closing, "a failed registry removal must not leave the signal closed");

	signal->id = id;
	acquired   = signal_acquire(id);
	cr_assert_eq(acquired, signal, "the registry entry must remain acquirable after a failed destroy");
	signal_release(acquired);
	cr_assert_eq(signal_destroy(signal), SIGNAL_OK);
}

Test(signal, synchronous_receivers_track_the_latest_generation_independently) {
	struct signal*        signal;
	struct uthread        first;
	struct uthread        second;
	struct signal_payload payload = {
		.args = {10u, 20u, 30u, 40u}
    };
	struct signal_message received;
	uint64_t              receiver_count;
	uint64_t              delivery_count;

	ipc_test_init_heap();
	signal_test_init_bound_bootstrap_cpu();
	signal_test_init_sched_uthread(&first, "signal_first", 0x420000u, 0x424000u, 0x9000u);
	signal_test_init_sched_uthread(&second, "signal_second", 0x430000u, 0x434000u, 0xa000u);
	signal = signal_create();
	cr_assert_not_null(signal);

	cr_assert_eq(signal_send(signal, SIGNAL_TEST_SENDER, &payload, NULL, NULL), SIGNAL_OK);
	sched_set_current(cpu_current(), &first.thread);
	cr_assert_eq(signal_try_wait(signal, &received), SIGNAL_OK);
	cr_assert_eq(received.sender, SIGNAL_TEST_SENDER);
	cr_assert_eq(memcmp(&payload, &received.payload, sizeof(payload)), 0);
	cr_assert_eq(signal_try_wait(signal, &received), SIGNAL_WOULD_BLOCK);

	sched_set_current(cpu_current(), &second.thread);
	cr_assert_eq(signal_try_wait(signal, &received), SIGNAL_OK);
	cr_assert_eq(received.sender, SIGNAL_TEST_SENDER);
	cr_assert_eq(memcmp(&payload, &received.payload, sizeof(payload)), 0);
	cr_assert_eq(signal_wait_subscription_count(signal), 2u);

	payload.args[0] = 99u;
	cr_assert_eq(signal_send(signal, SIGNAL_TEST_SENDER, &payload, &receiver_count, &delivery_count), SIGNAL_OK);
	cr_assert_eq(receiver_count, 0u, "inactive synchronous cursors are not immediate receivers");
	cr_assert_eq(delivery_count, 0u);

	sched_set_current(cpu_current(), &first.thread);
	cr_assert_eq(signal_try_wait(signal, &received), SIGNAL_OK);
	cr_assert_eq(received.sender, SIGNAL_TEST_SENDER);
	cr_assert_eq(received.payload.args[0], 99u);
	sched_set_current(cpu_current(), &second.thread);
	cr_assert_eq(signal_try_wait(signal, &received), SIGNAL_OK);
	cr_assert_eq(received.sender, SIGNAL_TEST_SENDER);
	cr_assert_eq(received.payload.args[0], 99u);
	cr_assert_eq(signal_read(signal, &received), SIGNAL_OK);
	cr_assert_eq(received.sender, SIGNAL_TEST_SENDER);
	cr_assert_eq(received.payload.args[0], 99u);
	cr_assert_eq(signal_try_wait(signal, &received), SIGNAL_WOULD_BLOCK, "read must not change the synchronous cursor");

	cr_assert_eq(signal_destroy(signal), SIGNAL_OK);
	signal_test_deinit_uthread(&first);
	signal_test_deinit_uthread(&second);
	signal_test_reset_scheduler_state();
}

Test(signal, handlers_receive_broadcast_and_report_partial_delivery) {
	struct signal*        signal;
	struct uthread        first;
	struct uthread        second;
	struct signal_payload payload = {
		.args = {5u, 6u, 7u, 8u}
    };
	struct user_upcall_request filler = {.entry = 0x3000u};
	uint64_t                   receiver_count;
	uint64_t                   delivery_count;

	ipc_test_init_heap();
	signal = signal_create();
	cr_assert_not_null(signal);
	signal_test_init_handler_uthread(&first, 0x9000u);
	signal_test_init_handler_uthread(&second, 0xa000u);
	cr_assert_eq(signal_register_handler(signal, &first, signal_test_handler, SIGNAL_HANDLER_FLAG_NONE), SIGNAL_OK);
	cr_assert_eq(signal_register_handler(signal, &second, signal_test_handler, SIGNAL_HANDLER_FLAG_NONE), SIGNAL_OK);

	for (size_t i = 0u; i < USER_UPCALL_QUEUE_CAPACITY; i++) {
		cr_assert_eq(uthread_upcall_enqueue(&second, &filler), USER_UPCALL_OK);
	}
	cr_assert_eq(signal_send(signal, SIGNAL_TEST_SENDER, &payload, &receiver_count, &delivery_count), SIGNAL_OK);
	cr_assert_eq(receiver_count, 2u);
	cr_assert_eq(delivery_count, 1u);
	cr_assert_eq(uthread_upcall_pending_count(&first), 1u);
	cr_assert_eq(first.upcall.pending[first.upcall.head].args[0], SIGNAL_TEST_SENDER);
	cr_assert_eq(first.upcall.pending[first.upcall.head].args[1], 5u);
	cr_assert_eq(first.upcall.pending[first.upcall.head].args[4], 8u);
	cr_assert_eq(uthread_upcall_pending_count(&second), USER_UPCALL_QUEUE_CAPACITY);

	cr_assert_eq(signal_destroy(signal), SIGNAL_OK);
	signal_test_deinit_uthread(&first);
	signal_test_deinit_uthread(&second);
}

Test(signal, coalesced_publications_replace_only_older_coalescible_publications) {
	struct signal*        signal;
	struct uthread        receiver;
	struct signal_payload payload = {
		.args = {1u, 2u, 3u, 4u}
    };
	size_t second;

	ipc_test_init_heap();
	signal = signal_create();
	cr_assert_not_null(signal);
	signal_test_init_handler_uthread(&receiver, 0x9000u);
	cr_assert_eq(signal_register_handler(signal, &receiver, signal_test_handler, SIGNAL_HANDLER_FLAG_NONE), SIGNAL_OK);

	cr_assert_eq(signal_send(signal, SIGNAL_TEST_SENDER, &payload, NULL, NULL), SIGNAL_OK);
	payload.args[0] = 10u;
	cr_assert_eq(signal_send_coalesced(signal, SIGNAL_TEST_SECOND_SENDER, &payload, NULL, NULL), SIGNAL_OK);
	payload.args[0] = 20u;
	cr_assert_eq(signal_send_coalesced(signal, SIGNAL_TEST_SENDER, &payload, NULL, NULL), SIGNAL_OK);

	cr_assert_eq(uthread_upcall_pending_count(&receiver), 2u);
	cr_assert_eq(receiver.upcall.pending[receiver.upcall.head].args[0], SIGNAL_TEST_SENDER);
	cr_assert_eq(receiver.upcall.pending[receiver.upcall.head].args[1], 1u);
	cr_assert_eq(receiver.upcall.pending[receiver.upcall.head].flags & USER_UPCALL_FLAG_COALESCIBLE, 0u);
	second = (receiver.upcall.head + 1u) % USER_UPCALL_QUEUE_CAPACITY;
	cr_assert_eq(receiver.upcall.pending[second].args[0], SIGNAL_TEST_SENDER);
	cr_assert_eq(receiver.upcall.pending[second].args[1], 20u);
	cr_assert((receiver.upcall.pending[second].flags & USER_UPCALL_FLAG_COALESCIBLE) != 0u);
	cr_assert_eq(uthread_upcall_dropped_count(&receiver), 0u);

	cr_assert_eq(signal_destroy(signal), SIGNAL_OK);
	signal_test_deinit_uthread(&receiver);
}

Test(signal, one_thread_can_receive_the_same_publication_by_handler_and_wait) {
	const struct thread_create_params sender_params = {
		.name              = "signal_sender",
		.entry             = signal_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x440000u,
		.kernel_stack_top  = 0x444000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct signal*        signal;
	struct uthread        receiver;
	struct thread         sender;
	struct signal_message received;

	ipc_test_init_heap();
	signal_test_init_bound_bootstrap_cpu();
	signal_test_init_sched_uthread(&receiver, "signal_receiver", 0x450000u, 0x454000u, 0x9000u);
	cr_assert(thread_init(&sender, &sender_params), "sender thread_init failed");
	cr_assert(sched_make_runnable(&sender), "sender should become runnable");

	signal = signal_create();
	cr_assert_not_null(signal);
	cr_assert_eq(signal_register_handler(signal, &receiver, signal_test_handler, SIGNAL_HANDLER_FLAG_NONE), SIGNAL_OK);
	signal_test_signal        = signal;
	signal_test_sender        = &sender;
	signal_test_first_payload = (struct signal_payload){
		.args = {11u, 22u, 33u, 44u}
    };
	hal_cpu_mock_set_context_switch_hook(signal_test_send_context_switch_hook);

	sched_set_current(cpu_current(), &receiver.thread);
	cr_assert_eq(signal_wait(signal, &received), SIGNAL_OK);
	cr_assert_eq(signal_test_hook_runs, 1u);
	cr_assert_eq(signal_test_first_receivers, 2u);
	cr_assert_eq(signal_test_first_deliveries, 2u);
	cr_assert_eq(received.sender, SIGNAL_TEST_SENDER);
	cr_assert_eq(memcmp(&received.payload, &signal_test_first_payload, sizeof(received.payload)), 0);
	cr_assert_eq(uthread_upcall_pending_count(&receiver), 1u);
	cr_assert_eq(receiver.upcall.pending[receiver.upcall.head].args[0], SIGNAL_TEST_SENDER);
	cr_assert_eq(receiver.upcall.pending[receiver.upcall.head].args[1], 11u);
	cr_assert_eq(signal_blocked_waiter_count(signal), 0u);

	cr_assert_eq(signal_destroy(signal), SIGNAL_OK);
	signal_test_deinit_uthread(&receiver);
	signal_test_reset_scheduler_state();
}

Test(signal, blocked_wait_keeps_its_wake_value_when_latest_advances) {
	const struct thread_create_params sender_params = {
		.name              = "signal_sender",
		.entry             = signal_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x460000u,
		.kernel_stack_top  = 0x464000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct signal*        signal;
	struct uthread        receiver;
	struct thread         sender;
	struct signal_message received;

	ipc_test_init_heap();
	signal_test_init_bound_bootstrap_cpu();
	signal_test_init_sched_uthread(&receiver, "signal_receiver", 0x470000u, 0x474000u, 0x9000u);
	cr_assert(thread_init(&sender, &sender_params), "sender thread_init failed");
	cr_assert(sched_make_runnable(&sender), "sender should become runnable");

	signal = signal_create();
	cr_assert_not_null(signal);
	signal_test_signal        = signal;
	signal_test_sender        = &sender;
	signal_test_first_payload = (struct signal_payload){
		.args = {1u, 2u, 3u, 4u}
    };
	signal_test_second_payload = (struct signal_payload){
		.args = {5u, 6u, 7u, 8u}
    };
	signal_test_send_twice = true;
	hal_cpu_mock_set_context_switch_hook(signal_test_send_context_switch_hook);

	sched_set_current(cpu_current(), &receiver.thread);
	cr_assert_eq(signal_wait(signal, &received), SIGNAL_OK);
	cr_assert_eq(received.sender, SIGNAL_TEST_SENDER);
	cr_assert_eq(received.payload.args[0], 1u, "the blocked wait must receive the value that woke it");
	cr_assert_eq(signal_test_first_receivers, 1u);
	cr_assert_eq(signal_test_first_deliveries, 1u);
	cr_assert_eq(signal_test_second_receivers, 0u);
	cr_assert_eq(signal_test_second_deliveries, 0u);
	cr_assert_eq(signal_try_wait(signal, &received), SIGNAL_OK);
	cr_assert_eq(received.sender, SIGNAL_TEST_SECOND_SENDER);
	cr_assert_eq(received.payload.args[0], 5u, "the next wait must observe the newer remembered value");
	cr_assert_eq(signal_try_wait(signal, &received), SIGNAL_WOULD_BLOCK);

	cr_assert_eq(signal_destroy(signal), SIGNAL_OK);
	signal_test_deinit_uthread(&receiver);
	signal_test_reset_scheduler_state();
}

Test(signal, thread_cleanup_removes_handlers_and_synchronous_cursors) {
	struct signal*        first;
	struct signal*        second;
	struct uthread        receiver;
	struct signal_message unused;

	ipc_test_init_heap();
	signal_test_init_bound_bootstrap_cpu();
	signal_test_init_sched_uthread(&receiver, "signal_receiver", 0x480000u, 0x484000u, 0x9000u);
	first  = signal_create();
	second = signal_create();
	cr_assert_not_null(first);
	cr_assert_not_null(second);

	cr_assert_eq(signal_register_handler(first, &receiver, signal_test_handler, SIGNAL_HANDLER_FLAG_NONE), SIGNAL_OK);
	cr_assert_eq(signal_register_handler(second, &receiver, signal_test_handler, SIGNAL_HANDLER_FLAG_NONE), SIGNAL_OK);
	sched_set_current(cpu_current(), &receiver.thread);
	cr_assert_eq(signal_try_wait(first, &unused), SIGNAL_WOULD_BLOCK);
	cr_assert_eq(signal_try_wait(second, &unused), SIGNAL_WOULD_BLOCK);
	cr_assert_eq(__atomic_load_n(&receiver.reference_count, __ATOMIC_ACQUIRE), 5u);

	signal_unregister_thread_receivers(&receiver);
	cr_assert_eq(signal_handler_count(first), 0u);
	cr_assert_eq(signal_handler_count(second), 0u);
	cr_assert_eq(signal_wait_subscription_count(first), 0u);
	cr_assert_eq(signal_wait_subscription_count(second), 0u);
	cr_assert_eq(__atomic_load_n(&receiver.reference_count, __ATOMIC_ACQUIRE), 1u);

	cr_assert_eq(signal_destroy(first), SIGNAL_OK);
	cr_assert_eq(signal_destroy(second), SIGNAL_OK);
	signal_test_deinit_uthread(&receiver);
	signal_test_reset_scheduler_state();
}

Test(signal, unregister_handler_purges_only_its_queued_upcalls) {
	struct signal*        signal;
	struct uthread        receiver;
	struct signal_payload payload = {
		.args = {1u, 2u, 3u, 4u}
    };
	struct user_upcall_request unrelated = {
		.entry = 0x3000u,
		.args  = {99u},
	};

	ipc_test_init_heap();
	signal = signal_create();
	cr_assert_not_null(signal);
	signal_test_init_handler_uthread(&receiver, 0x9000u);
	cr_assert_eq(signal_register_handler(signal, &receiver, signal_test_handler, SIGNAL_HANDLER_FLAG_NONE), SIGNAL_OK);
	cr_assert_eq(uthread_upcall_enqueue(&receiver, &unrelated), USER_UPCALL_OK);
	cr_assert_eq(signal_send(signal, SIGNAL_TEST_SENDER, &payload, NULL, NULL), SIGNAL_OK);
	cr_assert_eq(uthread_upcall_pending_count(&receiver), 2u);

	cr_assert_eq(signal_unregister_handler(signal, &receiver), SIGNAL_OK);
	cr_assert_eq(signal_handler_count(signal), 0u);
	cr_assert_eq(uthread_upcall_pending_count(&receiver), 1u);
	cr_assert_eq(receiver.upcall.pending[receiver.upcall.head].entry, unrelated.entry);
	cr_assert_eq(receiver.upcall.pending[receiver.upcall.head].args[0], 99u);

	cr_assert_eq(signal_destroy(signal), SIGNAL_OK);
	signal_test_deinit_uthread(&receiver);
}

Test(signal, handler_registration_is_immutable_and_clear_allows_recreate) {
	struct signal*        signal;
	struct uthread        receiver;
	struct signal_payload payload = {
		.args = {5u, 6u, 7u, 8u}
    };

	ipc_test_init_heap();
	signal = signal_create();
	cr_assert_not_null(signal);
	signal_test_init_handler_uthread(&receiver, 0x9000u);
	cr_assert_eq(signal_register_handler(signal, &receiver, signal_test_handler, SIGNAL_HANDLER_FLAG_NONE), SIGNAL_OK);
	cr_assert_eq(signal_send(signal, SIGNAL_TEST_SENDER, &payload, NULL, NULL), SIGNAL_OK);
	cr_assert_eq(uthread_upcall_pending_count(&receiver), 1u);
	cr_assert_eq(receiver.upcall.pending[receiver.upcall.head].entry, (uintptr_t)signal_test_handler);

	cr_assert_eq(signal_register_handler(signal, &receiver, signal_test_replacement_handler, SIGNAL_HANDLER_FLAG_NONE),
	             SIGNAL_HANDLER_ALREADY_REGISTERED);
	cr_assert_eq(uthread_upcall_pending_count(&receiver), 1u);
	cr_assert_eq(receiver.upcall.pending[receiver.upcall.head].entry, (uintptr_t)signal_test_handler);
	cr_assert_eq(signal_unregister_handler(signal, &receiver), SIGNAL_OK);
	cr_assert_eq(uthread_upcall_pending_count(&receiver), 0u);
	cr_assert_eq(signal_register_handler(signal, &receiver, signal_test_replacement_handler, SIGNAL_HANDLER_FLAG_NONE),
	             SIGNAL_OK);
	cr_assert_eq(signal_send(signal, SIGNAL_TEST_SENDER, &payload, NULL, NULL), SIGNAL_OK);
	cr_assert_eq(uthread_upcall_pending_count(&receiver), 1u);
	cr_assert_eq(receiver.upcall.pending[receiver.upcall.head].entry, (uintptr_t)signal_test_replacement_handler);

	cr_assert_eq(signal_destroy(signal), SIGNAL_OK);
	cr_assert_eq(uthread_upcall_pending_count(&receiver), 0u);
	signal_test_deinit_uthread(&receiver);
}

Test(signal, current_thread_can_unregister_and_recreate_synchronous_receiver) {
	struct signal*        signal;
	struct uthread        receiver;
	struct signal_payload payload = {
		.args = {9u, 8u, 7u, 6u}
    };
	struct signal_message received;

	ipc_test_init_heap();
	signal_test_init_bound_bootstrap_cpu();
	signal_test_init_sched_uthread(&receiver, "signal_receiver", 0x490000u, 0x494000u, 0x9000u);
	signal = signal_create();
	cr_assert_not_null(signal);
	sched_set_current(cpu_current(), &receiver.thread);

	cr_assert_eq(signal_unregister_wait_receiver(signal), SIGNAL_WAIT_RECEIVER_NOT_REGISTERED);
	cr_assert_eq(signal_try_wait(signal, &received), SIGNAL_WOULD_BLOCK);
	cr_assert_eq(signal_wait_subscription_count(signal), 1u);
	cr_assert_eq(__atomic_load_n(&receiver.reference_count, __ATOMIC_ACQUIRE), 2u);

	cr_assert_eq(signal_unregister_wait_receiver(signal), SIGNAL_OK);
	cr_assert_eq(signal_wait_subscription_count(signal), 0u);
	cr_assert_eq(__atomic_load_n(&receiver.reference_count, __ATOMIC_ACQUIRE), 1u);

	cr_assert_eq(signal_send(signal, (process_id_t)84u, &payload, NULL, NULL), SIGNAL_OK);
	cr_assert_eq(signal_try_wait(signal, &received), SIGNAL_OK);
	cr_assert_eq(received.sender, (process_id_t)84u);
	cr_assert_eq(memcmp(&received.payload, &payload, sizeof(payload)), 0);
	cr_assert_eq(signal_wait_subscription_count(signal), 1u);
	cr_assert_eq(__atomic_load_n(&receiver.reference_count, __ATOMIC_ACQUIRE), 2u);

	cr_assert_eq(signal_unregister_wait_receiver(signal), SIGNAL_OK);
	cr_assert_eq(signal_wait_subscription_count(signal), 0u);
	cr_assert_eq(signal_destroy(signal), SIGNAL_OK);
	signal_test_deinit_uthread(&receiver);
	signal_test_reset_scheduler_state();
}

Test(signal, queued_handler_interrupts_blocking_wait_on_another_signal) {
	const struct thread_create_params sender_params = {
		.name              = "signal_upcall_sender",
		.entry             = signal_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x4a0000u,
		.kernel_stack_top  = 0x4a4000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	struct signal*        handler_signal;
	struct signal*        waited_signal;
	struct uthread        receiver;
	struct thread         sender;
	struct signal_message unused;

	ipc_test_init_heap();
	signal_test_init_bound_bootstrap_cpu();
	signal_test_init_sched_uthread(&receiver, "signal_handler_waiter", 0x4b0000u, 0x4b4000u, 0x9000u);
	cr_assert(thread_init(&sender, &sender_params), "sender thread_init failed");
	cr_assert(sched_make_runnable(&sender), "sender should become runnable");

	handler_signal = signal_create();
	waited_signal  = signal_create();
	cr_assert_not_null(handler_signal);
	cr_assert_not_null(waited_signal);
	cr_assert_eq(signal_register_handler(handler_signal, &receiver, signal_test_handler, SIGNAL_HANDLER_FLAG_NONE),
	             SIGNAL_OK);

	signal_test_signal        = handler_signal;
	signal_test_sender        = &sender;
	signal_test_first_payload = (struct signal_payload){
		.args = {1u, 2u, 3u, 4u}
    };
	hal_cpu_mock_set_context_switch_hook(signal_test_send_context_switch_hook);

	sched_set_current(cpu_current(), &receiver.thread);
	cr_assert_eq(signal_wait(waited_signal, &unused), SIGNAL_WAIT_INTERRUPTED);
	cr_assert_eq(signal_test_hook_runs, 1u);
	cr_assert_eq(signal_test_first_receivers, 1u);
	cr_assert_eq(signal_test_first_deliveries, 1u);
	cr_assert_eq(signal_blocked_waiter_count(waited_signal), 0u);
	cr_assert_eq(uthread_upcall_pending_count(&receiver), 1u);
	cr_assert(thread_interrupt_pending(&receiver.thread));

	cr_assert_eq(signal_unregister_wait_receiver(waited_signal), SIGNAL_OK);
	cr_assert_eq(signal_destroy(handler_signal), SIGNAL_OK);
	cr_assert_eq(uthread_upcall_pending_count(&receiver), 0u);
	cr_assert_not(thread_interrupt_pending(&receiver.thread));
	cr_assert_eq(signal_destroy(waited_signal), SIGNAL_OK);
	signal_test_deinit_uthread(&receiver);
	signal_test_reset_scheduler_state();
}

Test(signal, forced_send_aborts_before_publication_when_one_handler_has_only_protected_upcalls) {
	struct signal*        signal;
	struct uthread        available;
	struct uthread        blocked;
	struct signal_payload payload = {
		.args = {10u, 20u, 30u, 40u}
    };
	struct user_upcall_request protected = {
		.flags = USER_UPCALL_FLAG_NON_EVICTABLE,
		.entry = 0x3000u,
	};
	uint64_t receiver_count = UINT64_MAX;
	uint64_t delivery_count = UINT64_MAX;

	ipc_test_init_heap();
	signal = signal_create();
	cr_assert_not_null(signal);
	signal_test_init_handler_uthread(&available, 0x9000u);
	signal_test_init_handler_uthread(&blocked, 0xa000u);
	cr_assert_eq(signal_register_handler(signal, &available, signal_test_handler, SIGNAL_HANDLER_FLAG_NONE), SIGNAL_OK);
	cr_assert_eq(signal_register_handler(signal, &blocked, signal_test_handler, SIGNAL_HANDLER_FLAG_NONE), SIGNAL_OK);

	for (size_t i = 0u; i < USER_UPCALL_QUEUE_CAPACITY; i++) {
		protected.args[0] = i;
		cr_assert_eq(uthread_upcall_enqueue(&blocked, &protected), USER_UPCALL_OK);
	}
	cr_assert_eq(signal_send_force(signal, SIGNAL_TEST_SENDER, &payload, &receiver_count, &delivery_count),
	             SIGNAL_UNAVAILABLE);
	cr_assert_eq(receiver_count, 0u);
	cr_assert_eq(delivery_count, 0u);
	cr_assert_not(signal_has_value(signal));
	cr_assert_eq(signal_generation(signal), 0u);
	cr_assert_eq(uthread_upcall_pending_count(&available), 0u);
	cr_assert_eq(available.upcall.force_free_reservations, 0u);
	cr_assert_eq(available.upcall.force_eviction_reservations, 0u);
	cr_assert_eq(uthread_upcall_pending_count(&blocked), USER_UPCALL_QUEUE_CAPACITY);
	cr_assert_eq(uthread_upcall_dropped_count(&available), 0u);
	cr_assert_eq(uthread_upcall_dropped_count(&blocked), 0u);

	cr_assert_eq(signal_destroy(signal), SIGNAL_OK);
	signal_test_deinit_uthread(&available);
	signal_test_deinit_uthread(&blocked);
}

Test(signal, forced_send_evicts_an_old_upcall_and_protects_the_signal_delivery) {
	struct signal*        signal;
	struct uthread        receiver;
	struct signal_payload payload = {
		.args = {11u, 22u, 33u, 44u}
    };
	struct signal_message      received;
	struct user_upcall_request protected = {
		.flags = USER_UPCALL_FLAG_NON_EVICTABLE,
		.entry = 0x3000u,
	};
	struct user_upcall_request evictable = {
		.entry = 0x4000u,
		.args  = {0xdeadu},
	};
	uint64_t receiver_count;
	uint64_t delivery_count;
	size_t   tail;

	ipc_test_init_heap();
	signal = signal_create();
	cr_assert_not_null(signal);
	signal_test_init_handler_uthread(&receiver, 0x9000u);
	cr_assert_eq(signal_register_handler(signal, &receiver, signal_test_handler, SIGNAL_HANDLER_FLAG_NONE), SIGNAL_OK);

	protected.args[0] = 1u;
	cr_assert_eq(uthread_upcall_enqueue(&receiver, &protected), USER_UPCALL_OK);
	cr_assert_eq(uthread_upcall_enqueue(&receiver, &evictable), USER_UPCALL_OK);
	for (size_t i = 2u; i < USER_UPCALL_QUEUE_CAPACITY; i++) {
		protected.args[0] = i + 1u;
		cr_assert_eq(uthread_upcall_enqueue(&receiver, &protected), USER_UPCALL_OK);
	}

	cr_assert_eq(signal_send_force(signal, SIGNAL_TEST_SENDER, &payload, &receiver_count, &delivery_count), SIGNAL_OK);
	cr_assert_eq(receiver_count, 1u);
	cr_assert_eq(delivery_count, 1u);
	cr_assert_eq(uthread_upcall_pending_count(&receiver), USER_UPCALL_QUEUE_CAPACITY);
	cr_assert_eq(uthread_upcall_dropped_count(&receiver), 1u);
	cr_assert_eq(signal_generation(signal), 1u);
	cr_assert_eq(signal_read(signal, &received), SIGNAL_OK);
	cr_assert_eq(received.sender, SIGNAL_TEST_SENDER);
	cr_assert_eq(memcmp(&received.payload, &payload, sizeof(payload)), 0);

	tail = (receiver.upcall.head + receiver.upcall.count - 1u) % USER_UPCALL_QUEUE_CAPACITY;
	cr_assert_eq(receiver.upcall.pending[tail].entry, (uintptr_t)signal_test_handler);
	cr_assert((receiver.upcall.pending[tail].flags & USER_UPCALL_FLAG_NON_EVICTABLE) != 0u);
	cr_assert_eq(receiver.upcall.pending[tail].args[0], SIGNAL_TEST_SENDER);
	cr_assert_eq(receiver.upcall.pending[tail].args[1], 11u);
	for (size_t i = 0u; i < receiver.upcall.count; i++) {
		size_t index = (receiver.upcall.head + i) % USER_UPCALL_QUEUE_CAPACITY;

		cr_assert_neq(receiver.upcall.pending[index].args[0], 0xdeadu, "evictable request must be replaced");
	}

	cr_assert_eq(signal_destroy(signal), SIGNAL_OK);
	signal_test_deinit_uthread(&receiver);
}

Test(signal, oneshot_handler_detaches_only_after_successful_admission) {
	struct signal*        signal;
	struct uthread        receiver;
	struct signal_payload first = {
		.args = {10u, 20u, 30u, 40u}
    };
	struct signal_payload second = {
		.args = {50u, 60u, 70u, 80u}
    };
	uint64_t receiver_count;
	uint64_t delivery_count;

	ipc_test_init_heap();
	signal = signal_create();
	cr_assert_not_null(signal);
	signal_test_init_handler_uthread(&receiver, 0x9000u);
	cr_assert_eq(signal_register_handler(signal, &receiver, signal_test_handler, SIGNAL_HANDLER_FLAG_ONESHOT),
	             SIGNAL_OK);
	cr_assert_eq(signal_handler_count(signal), 1u);

	cr_assert_eq(signal_send(signal, SIGNAL_TEST_SENDER, &first, &receiver_count, &delivery_count), SIGNAL_OK);
	cr_assert_eq(receiver_count, 1u);
	cr_assert_eq(delivery_count, 1u);
	cr_assert_eq(signal_handler_count(signal), 0u);
	cr_assert_eq(uthread_upcall_pending_count(&receiver), 1u);
	cr_assert_eq(receiver.upcall.pending[receiver.upcall.head].origin, USER_UPCALL_ORIGIN_NONE);
	cr_assert_eq(receiver.upcall.pending[receiver.upcall.head].origin_token, 0u);
	cr_assert((receiver.upcall.pending[receiver.upcall.head].flags & USER_UPCALL_FLAG_NON_EVICTABLE) != 0u);
	cr_assert_eq(receiver.upcall.pending[receiver.upcall.head].args[0], SIGNAL_TEST_SENDER);
	cr_assert_eq(receiver.upcall.pending[receiver.upcall.head].args[1], 10u);

	cr_assert_eq(signal_send(signal, SIGNAL_TEST_SECOND_SENDER, &second, &receiver_count, &delivery_count), SIGNAL_OK);
	cr_assert_eq(receiver_count, 0u);
	cr_assert_eq(delivery_count, 0u);
	cr_assert_eq(uthread_upcall_pending_count(&receiver), 1u);

	cr_assert_eq(signal_destroy(signal), SIGNAL_OK);
	cr_assert_eq(uthread_upcall_pending_count(&receiver), 1u, "an admitted one-shot must survive the Signal lifecycle");
	signal_test_deinit_uthread(&receiver);
}

Test(signal, oneshot_handler_reuses_retired_binding_when_rearmed) {
	struct signal*        signal;
	struct uthread        receiver;
	struct signal_payload payload = {
		.args = {1u, 2u, 3u, 4u}
    };

	ipc_test_init_heap();
	signal = signal_create();
	cr_assert_not_null(signal);
	signal_test_init_handler_uthread(&receiver, 0x9000u);
	cr_assert_eq(__atomic_load_n(&receiver.reference_count, __ATOMIC_ACQUIRE), 1u);

	for (size_t i = 0u; i < 3u; i++) {
		cr_assert_eq(signal_register_handler(signal, &receiver, signal_test_handler, SIGNAL_HANDLER_FLAG_ONESHOT),
		             SIGNAL_OK);
		cr_assert_eq(signal_handler_count(signal), 1u);
		cr_assert_eq(__atomic_load_n(&receiver.reference_count, __ATOMIC_ACQUIRE), 2u);

		payload.args[0] = i + 1u;
		cr_assert_eq(signal_send_force(signal, SIGNAL_TEST_SENDER, &payload, NULL, NULL), SIGNAL_OK);
		cr_assert_eq(signal_handler_count(signal), 0u);
		cr_assert_eq(__atomic_load_n(&receiver.reference_count, __ATOMIC_ACQUIRE),
		             2u,
		             "a consumed one-shot binding stays retained until safe lifecycle reclamation");
	}

	cr_assert_eq(uthread_upcall_pending_count(&receiver), 3u);
	cr_assert_eq(signal_unregister_handler(signal, &receiver), SIGNAL_HANDLER_NOT_REGISTERED);
	cr_assert_eq(signal_destroy(signal), SIGNAL_OK);
	cr_assert_eq(__atomic_load_n(&receiver.reference_count, __ATOMIC_ACQUIRE), 1u);
	cr_assert_eq(uthread_upcall_pending_count(&receiver), 3u);
	signal_test_deinit_uthread(&receiver);
}

Test(signal, full_queue_keeps_oneshot_handler_armed) {
	struct signal*        signal;
	struct uthread        receiver;
	struct signal_payload payload = {
		.args = {1u, 2u, 3u, 4u}
    };
	struct user_upcall_request filler = {
		.entry = 0x3000u,
	};
	uint64_t receiver_count;
	uint64_t delivery_count;

	ipc_test_init_heap();
	signal = signal_create();
	cr_assert_not_null(signal);
	signal_test_init_handler_uthread(&receiver, 0x9000u);
	cr_assert_eq(signal_register_handler(signal, &receiver, signal_test_handler, SIGNAL_HANDLER_FLAG_ONESHOT),
	             SIGNAL_OK);

	for (size_t i = 0u; i < USER_UPCALL_QUEUE_CAPACITY; i++) {
		filler.args[0] = i;
		cr_assert_eq(uthread_upcall_enqueue(&receiver, &filler), USER_UPCALL_OK);
	}
	cr_assert_eq(signal_send(signal, SIGNAL_TEST_SENDER, &payload, &receiver_count, &delivery_count), SIGNAL_OK);
	cr_assert_eq(receiver_count, 1u);
	cr_assert_eq(delivery_count, 0u);
	cr_assert_eq(signal_handler_count(signal), 1u);
	cr_assert_eq(uthread_upcall_pending_count(&receiver), USER_UPCALL_QUEUE_CAPACITY);
	cr_assert_eq(uthread_upcall_dropped_count(&receiver), 1u);

	cr_assert_eq(signal_unregister_handler(signal, &receiver), SIGNAL_OK);
	cr_assert_eq(signal_destroy(signal), SIGNAL_OK);
	signal_test_deinit_uthread(&receiver);
}

Test(signal, coalesced_oneshot_is_admitted_as_an_independent_request) {
	struct signal*        signal;
	struct uthread        receiver;
	struct signal_payload payload = {
		.args = {7u, 8u, 9u, 10u}
    };

	ipc_test_init_heap();
	signal = signal_create();
	cr_assert_not_null(signal);
	signal_test_init_handler_uthread(&receiver, 0x9000u);
	cr_assert_eq(signal_register_handler(signal, &receiver, signal_test_handler, SIGNAL_HANDLER_FLAG_ONESHOT),
	             SIGNAL_OK);

	cr_assert_eq(signal_send_coalesced(signal, SIGNAL_TEST_SENDER, &payload, NULL, NULL), SIGNAL_OK);
	cr_assert_eq(signal_handler_count(signal), 0u);
	cr_assert_eq(uthread_upcall_pending_count(&receiver), 1u);
	cr_assert_eq(receiver.upcall.pending[receiver.upcall.head].origin, USER_UPCALL_ORIGIN_NONE);
	cr_assert_eq(receiver.upcall.pending[receiver.upcall.head].origin_token, 0u);
	cr_assert((receiver.upcall.pending[receiver.upcall.head].flags & USER_UPCALL_FLAG_COALESCIBLE) != 0u);
	cr_assert((receiver.upcall.pending[receiver.upcall.head].flags & USER_UPCALL_FLAG_NON_EVICTABLE) != 0u);

	cr_assert_eq(signal_destroy(signal), SIGNAL_OK);
	cr_assert_eq(uthread_upcall_pending_count(&receiver), 1u);
	signal_test_deinit_uthread(&receiver);
}

Test(signal, forced_oneshot_is_protected_and_survives_detach) {
	struct signal*        signal;
	struct uthread        receiver;
	struct signal_payload payload = {
		.args = {11u, 22u, 33u, 44u}
    };
	struct user_upcall_request protected = {
		.flags = USER_UPCALL_FLAG_NON_EVICTABLE,
		.entry = 0x3000u,
	};
	struct user_upcall_request evictable = {
		.entry = 0x4000u,
		.args  = {0xdeadu},
	};
	uint64_t receiver_count;
	uint64_t delivery_count;
	size_t   tail;

	ipc_test_init_heap();
	signal = signal_create();
	cr_assert_not_null(signal);
	signal_test_init_handler_uthread(&receiver, 0x9000u);
	cr_assert_eq(signal_register_handler(signal, &receiver, signal_test_handler, SIGNAL_HANDLER_FLAG_ONESHOT),
	             SIGNAL_OK);

	cr_assert_eq(uthread_upcall_enqueue(&receiver, &evictable), USER_UPCALL_OK);
	for (size_t i = 1u; i < USER_UPCALL_QUEUE_CAPACITY; i++) {
		protected.args[0] = i;
		cr_assert_eq(uthread_upcall_enqueue(&receiver, &protected), USER_UPCALL_OK);
	}
	cr_assert_eq(signal_send_force(signal, SIGNAL_TEST_SENDER, &payload, &receiver_count, &delivery_count), SIGNAL_OK);
	cr_assert_eq(receiver_count, 1u);
	cr_assert_eq(delivery_count, 1u);
	cr_assert_eq(signal_handler_count(signal), 0u);
	cr_assert_eq(uthread_upcall_pending_count(&receiver), USER_UPCALL_QUEUE_CAPACITY);
	cr_assert_eq(uthread_upcall_dropped_count(&receiver), 1u);
	tail = (receiver.upcall.head + receiver.upcall.count - 1u) % USER_UPCALL_QUEUE_CAPACITY;
	cr_assert_eq(receiver.upcall.pending[tail].origin, USER_UPCALL_ORIGIN_NONE);
	cr_assert_eq(receiver.upcall.pending[tail].origin_token, 0u);
	cr_assert((receiver.upcall.pending[tail].flags & USER_UPCALL_FLAG_NON_EVICTABLE) != 0u);
	cr_assert_eq(receiver.upcall.pending[tail].args[0], SIGNAL_TEST_SENDER);
	cr_assert_eq(receiver.upcall.pending[tail].args[1], 11u);

	cr_assert_eq(signal_destroy(signal), SIGNAL_OK);
	cr_assert_eq(uthread_upcall_pending_count(&receiver), USER_UPCALL_QUEUE_CAPACITY);
	signal_test_deinit_uthread(&receiver);
}

Test(signal, forced_preflight_failure_leaves_blocked_waiter_untouched) {
	const struct thread_create_params sender_params = {
		.name              = "signal_force_sender",
		.entry             = signal_test_thread_entry,
		.arg               = NULL,
		.kernel_stack_base = 0x4c0000u,
		.kernel_stack_top  = 0x4c4000u,
		.preferred_cpu     = NULL,
		.detached          = false,
	};
	const uintptr_t            purge_token = 0xabcdu;
	struct signal*             signal;
	struct uthread             receiver;
	struct uthread             blocked_handler;
	struct thread              sender;
	struct signal_message      received;
	struct user_upcall_request protected = {
		.flags = USER_UPCALL_FLAG_NON_EVICTABLE,
		.entry = 0x3000u,
	};

	ipc_test_init_heap();
	signal_test_init_bound_bootstrap_cpu();
	signal_test_init_sched_uthread(&receiver, "signal_force_waiter", 0x4d0000u, 0x4d4000u, 0x9000u);
	signal_test_init_handler_uthread(&blocked_handler, 0xa000u);
	cr_assert(thread_init(&sender, &sender_params), "sender thread_init failed");
	cr_assert(sched_make_runnable(&sender), "sender should become runnable");

	signal = signal_create();
	cr_assert_not_null(signal);
	cr_assert_eq(signal_register_handler(signal, &blocked_handler, signal_test_handler, SIGNAL_HANDLER_FLAG_NONE),
	             SIGNAL_OK);
	protected.origin       = USER_UPCALL_ORIGIN_SIGNAL;
	protected.origin_token = purge_token;
	protected.args[0]      = 1u;
	cr_assert_eq(uthread_upcall_enqueue(&blocked_handler, &protected), USER_UPCALL_OK);
	protected.origin       = USER_UPCALL_ORIGIN_NONE;
	protected.origin_token = 0u;
	for (size_t i = 1u; i < USER_UPCALL_QUEUE_CAPACITY; i++) {
		protected.args[0] = i + 1u;
		cr_assert_eq(uthread_upcall_enqueue(&blocked_handler, &protected), USER_UPCALL_OK);
	}

	signal_test_signal        = signal;
	signal_test_sender        = &sender;
	signal_test_first_payload = (struct signal_payload){
		.args = {10u, 20u, 30u, 40u}
    };
	signal_test_second_payload = (struct signal_payload){
		.args = {50u, 60u, 70u, 80u}
    };
	signal_test_force_retry           = true;
	signal_test_force_blocked_handler = &blocked_handler;
	signal_test_force_purge_token     = purge_token;
	hal_cpu_mock_set_context_switch_hook(signal_test_send_context_switch_hook);

	sched_set_current(cpu_current(), &receiver.thread);
	cr_assert_eq(signal_wait(signal, &received), SIGNAL_OK);
	cr_assert_eq(signal_test_hook_runs, 1u);
	cr_assert_eq(signal_test_first_receivers, 0u);
	cr_assert_eq(signal_test_first_deliveries, 0u);
	cr_assert_eq(signal_test_second_receivers, 2u);
	cr_assert_eq(signal_test_second_deliveries, 2u);
	cr_assert_eq(received.sender, SIGNAL_TEST_SECOND_SENDER);
	cr_assert_eq(memcmp(&received.payload, &signal_test_second_payload, sizeof(received.payload)), 0);
	cr_assert_eq(signal_generation(signal), 1u);
	cr_assert_eq(signal_blocked_waiter_count(signal), 0u);
	cr_assert_eq(uthread_upcall_pending_count(&blocked_handler), USER_UPCALL_QUEUE_CAPACITY);
	cr_assert_eq(uthread_upcall_dropped_count(&blocked_handler), 0u);

	cr_assert_eq(signal_destroy(signal), SIGNAL_OK);
	signal_test_deinit_uthread(&blocked_handler);
	signal_test_deinit_uthread(&receiver);
	signal_test_reset_scheduler_state();
}

Test(signal, failed_forced_oneshot_preflight_keeps_handler_armed_for_retry) {
	const uintptr_t       purge_token = 0xbeefu;
	struct signal*        signal;
	struct uthread        receiver;
	struct signal_payload payload = {
		.args = {1u, 2u, 3u, 4u}
    };
	struct user_upcall_request protected = {
		.flags = USER_UPCALL_FLAG_NON_EVICTABLE,
		.entry = 0x3000u,
	};
	uint64_t receiver_count = UINT64_MAX;
	uint64_t delivery_count = UINT64_MAX;
	size_t   tail;

	ipc_test_init_heap();
	signal = signal_create();
	cr_assert_not_null(signal);
	signal_test_init_handler_uthread(&receiver, 0x9000u);
	cr_assert_eq(signal_register_handler(signal, &receiver, signal_test_handler, SIGNAL_HANDLER_FLAG_ONESHOT),
	             SIGNAL_OK);
	protected.origin       = USER_UPCALL_ORIGIN_SIGNAL;
	protected.origin_token = purge_token;
	cr_assert_eq(uthread_upcall_enqueue(&receiver, &protected), USER_UPCALL_OK);
	protected.origin       = USER_UPCALL_ORIGIN_NONE;
	protected.origin_token = 0u;
	for (size_t i = 1u; i < USER_UPCALL_QUEUE_CAPACITY; i++) {
		protected.args[0] = i;
		cr_assert_eq(uthread_upcall_enqueue(&receiver, &protected), USER_UPCALL_OK);
	}

	cr_assert_eq(signal_send_force(signal, SIGNAL_TEST_SENDER, &payload, &receiver_count, &delivery_count),
	             SIGNAL_UNAVAILABLE);
	cr_assert_eq(receiver_count, 0u);
	cr_assert_eq(delivery_count, 0u);
	cr_assert_eq(signal_handler_count(signal), 1u);
	cr_assert_eq(signal_generation(signal), 0u);
	cr_assert_eq(__atomic_load_n(&receiver.reference_count, __ATOMIC_ACQUIRE), 2u);
	cr_assert_eq(uthread_upcall_dropped_count(&receiver), 0u);
	cr_assert_eq(uthread_upcall_purge(&receiver, USER_UPCALL_ORIGIN_SIGNAL, purge_token), 1u);

	payload.args[0] = 99u;
	cr_assert_eq(signal_send_force(signal, SIGNAL_TEST_SECOND_SENDER, &payload, &receiver_count, &delivery_count),
	             SIGNAL_OK);
	cr_assert_eq(receiver_count, 1u);
	cr_assert_eq(delivery_count, 1u);
	cr_assert_eq(signal_handler_count(signal), 0u);
	cr_assert_eq(signal_generation(signal), 1u);
	cr_assert_eq(uthread_upcall_pending_count(&receiver), USER_UPCALL_QUEUE_CAPACITY);
	tail = (receiver.upcall.head + receiver.upcall.count - 1u) % USER_UPCALL_QUEUE_CAPACITY;
	cr_assert_eq(receiver.upcall.pending[tail].origin, USER_UPCALL_ORIGIN_NONE);
	cr_assert_eq(receiver.upcall.pending[tail].args[0], SIGNAL_TEST_SECOND_SENDER);
	cr_assert_eq(receiver.upcall.pending[tail].args[1], 99u);
	cr_assert((receiver.upcall.pending[tail].flags & USER_UPCALL_FLAG_NON_EVICTABLE) != 0u);

	cr_assert_eq(signal_destroy(signal), SIGNAL_OK);
	cr_assert_eq(__atomic_load_n(&receiver.reference_count, __ATOMIC_ACQUIRE), 1u);
	cr_assert_eq(uthread_upcall_pending_count(&receiver), USER_UPCALL_QUEUE_CAPACITY);
	signal_test_deinit_uthread(&receiver);
}

Test(signal, thread_cleanup_reclaims_retired_oneshot_without_purging_admitted_delivery) {
	struct signal*        signal;
	struct uthread        receiver;
	struct signal_payload payload = {
		.args = {7u, 8u, 9u, 10u}
    };

	ipc_test_init_heap();
	signal = signal_create();
	cr_assert_not_null(signal);
	signal_test_init_handler_uthread(&receiver, 0x9000u);
	cr_assert_eq(signal_register_handler(signal, &receiver, signal_test_handler, SIGNAL_HANDLER_FLAG_ONESHOT),
	             SIGNAL_OK);
	cr_assert_eq(signal_send_force(signal, SIGNAL_TEST_SENDER, &payload, NULL, NULL), SIGNAL_OK);
	cr_assert_eq(signal_handler_count(signal), 0u);
	cr_assert_eq(__atomic_load_n(&receiver.reference_count, __ATOMIC_ACQUIRE), 2u);
	cr_assert_eq(uthread_upcall_pending_count(&receiver), 1u);

	signal_unregister_thread_receivers(&receiver);
	cr_assert_eq(__atomic_load_n(&receiver.reference_count, __ATOMIC_ACQUIRE), 1u);
	cr_assert_eq(uthread_upcall_pending_count(&receiver), 1u);
	cr_assert_eq(receiver.upcall.pending[receiver.upcall.head].origin, USER_UPCALL_ORIGIN_NONE);

	cr_assert_eq(signal_destroy(signal), SIGNAL_OK);
	cr_assert_eq(uthread_upcall_pending_count(&receiver), 1u);
	signal_test_deinit_uthread(&receiver);
}

Test(signal, rearmed_oneshot_binding_can_change_entry_and_become_persistent) {
	struct signal*        signal;
	struct uthread        receiver;
	struct signal_payload first = {
		.args = {1u, 2u, 3u, 4u}
    };
	struct signal_payload second = {
		.args = {5u, 6u, 7u, 8u}
    };
	size_t second_index;

	ipc_test_init_heap();
	signal = signal_create();
	cr_assert_not_null(signal);
	signal_test_init_handler_uthread(&receiver, 0x9000u);
	cr_assert_eq(signal_register_handler(signal, &receiver, signal_test_handler, SIGNAL_HANDLER_FLAG_ONESHOT),
	             SIGNAL_OK);
	cr_assert_eq(signal_send(signal, SIGNAL_TEST_SENDER, &first, NULL, NULL), SIGNAL_OK);
	cr_assert_eq(signal_handler_count(signal), 0u);

	cr_assert_eq(signal_register_handler(signal, &receiver, signal_test_replacement_handler, SIGNAL_HANDLER_FLAG_NONE),
	             SIGNAL_OK);
	cr_assert_eq(signal_handler_count(signal), 1u);
	cr_assert_eq(__atomic_load_n(&receiver.reference_count, __ATOMIC_ACQUIRE), 2u);
	cr_assert_eq(signal_send(signal, SIGNAL_TEST_SECOND_SENDER, &second, NULL, NULL), SIGNAL_OK);
	cr_assert_eq(uthread_upcall_pending_count(&receiver), 2u);
	cr_assert_eq(receiver.upcall.pending[receiver.upcall.head].origin, USER_UPCALL_ORIGIN_NONE);
	cr_assert_eq(receiver.upcall.pending[receiver.upcall.head].entry, (uintptr_t)signal_test_handler);
	second_index = (receiver.upcall.head + 1u) % USER_UPCALL_QUEUE_CAPACITY;
	cr_assert_eq(receiver.upcall.pending[second_index].origin, USER_UPCALL_ORIGIN_SIGNAL);
	cr_assert_neq(receiver.upcall.pending[second_index].origin_token, 0u);
	cr_assert_eq(receiver.upcall.pending[second_index].entry, (uintptr_t)signal_test_replacement_handler);
	cr_assert_eq(receiver.upcall.pending[second_index].flags & USER_UPCALL_FLAG_NON_EVICTABLE, 0u);

	cr_assert_eq(signal_unregister_handler(signal, &receiver), SIGNAL_OK);
	cr_assert_eq(__atomic_load_n(&receiver.reference_count, __ATOMIC_ACQUIRE), 1u);
	cr_assert_eq(uthread_upcall_pending_count(&receiver),
	             1u,
	             "clearing the rearmed persistent handler must not purge the admitted one-shot");
	cr_assert_eq(receiver.upcall.pending[receiver.upcall.head].entry, (uintptr_t)signal_test_handler);

	cr_assert_eq(signal_destroy(signal), SIGNAL_OK);
	cr_assert_eq(uthread_upcall_pending_count(&receiver), 1u);
	signal_test_deinit_uthread(&receiver);
}

Test(signal, direct_operations_use_specific_rights_without_cap_call) {
	struct signal*        signal;
	struct signal_payload payload = {
		.args = {12u, 34u, 56u, 78u}
    };
	struct signal_message             message;
	struct signal_send_response       response;
	struct signal_set_handler_request set_handler_request = {
		.header  = {.op = SIGNAL_OP_SET_HANDLER},
		.handler = signal_test_handler,
	};
	struct signal_read_request read_request = {
		.header = {.op = SIGNAL_OP_READ},
	};
	struct signal_read_response read_response;
	struct cap_request          cap_request;
	struct capability*          control;
	struct capability*          info;
	struct cap_object*          object;
	cap_id_t                    sender_cap;
	cap_id_t                    reader_cap;
	cap_id_t                    control_cap;
	cap_id_t                    info_cap;
	syscall_result_t            result;

	ipc_test_init_heap();
	capability_init();
	signal = signal_create();
	cr_assert_not_null(signal);
	sender_cap  = kernel_signal_grant(signal, SIGNAL_TEST_SENDER, CAP_SIGNAL);
	reader_cap  = kernel_signal_grant(signal, SIGNAL_TEST_SECOND_SENDER, CAP_READ);
	control_cap = kernel_signal_grant(signal, SIGNAL_TEST_SENDER, CAP_CALL);
	info_cap    = kernel_signal_grant(signal, SIGNAL_TEST_SECOND_SENDER, CAP_CALL | CAP_READ);
	cr_assert_neq(sender_cap, CAP_ID_INVALID);
	cr_assert_neq(reader_cap, CAP_ID_INVALID);
	cr_assert_neq(control_cap, CAP_ID_INVALID);
	cr_assert_neq(info_cap, CAP_ID_INVALID);

	result = kernel_signal_read(sender_cap, SIGNAL_TEST_SENDER, &message);
	cr_assert_eq(result.status, SYSCALL_STATUS_DENIED, "CAP_SIGNAL must not imply CAP_READ");
	result = kernel_signal_try_wait(sender_cap, SIGNAL_TEST_SENDER, &message);
	cr_assert_eq(result.status, SYSCALL_STATUS_DENIED, "CAP_SIGNAL must not imply CAP_WAIT");
	result = kernel_signal_send(reader_cap, SIGNAL_TEST_SECOND_SENDER, &payload, SIGNAL_SEND_FLAG_NONE, &response);
	cr_assert_eq(result.status, SYSCALL_STATUS_DENIED, "CAP_READ must not imply CAP_SIGNAL");
	result = kernel_signal_send(sender_cap, SIGNAL_TEST_SECOND_SENDER, &payload, SIGNAL_SEND_FLAG_NONE, &response);
	cr_assert_eq(result.status, SYSCALL_STATUS_DENIED, "a capability must remain bound to its recipient PID");

	control = cap_lookup(control_cap);
	cr_assert_not_null(control);
	object = cap_object_acquire(control->cap_object_id);
	cr_assert_not_null(object);
	cr_assert_not_null(object->handler);
	cap_request = (struct cap_request){
		.caller       = SIGNAL_TEST_SENDER,
		.cap_id       = control_cap,
		.object_id    = signal_id(signal),
		.rights       = control->rights,
		.request      = &set_handler_request,
		.request_size = sizeof(set_handler_request),
	};
	result = object->handler(&cap_request);
	cr_assert_eq(result.status, SYSCALL_STATUS_DENIED, "CAP_CALL must not imply the CAP_MAP handler right");
	cap_request = (struct cap_request){
		.caller            = SIGNAL_TEST_SENDER,
		.cap_id            = control_cap,
		.object_id         = signal_id(signal),
		.rights            = control->rights,
		.request           = &read_request,
		.request_size      = sizeof(read_request),
		.response          = &read_response,
		.response_capacity = sizeof(read_response),
	};
	result = object->handler(&cap_request);
	cr_assert_eq(result.status, SYSCALL_STATUS_DENIED, "CAP_CALL must not imply the CAP_READ handler right");
	cap_object_release(object);

	result = kernel_signal_send(sender_cap, SIGNAL_TEST_SENDER, &payload, SIGNAL_SEND_FLAG_NONE, &response);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(response.receiver_count, 0u);
	cr_assert_eq(response.delivery_count, 0u);
	result = kernel_signal_read(reader_cap, SIGNAL_TEST_SECOND_SENDER, &message);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(result.value, 1u);
	cr_assert_eq(message.sender, SIGNAL_TEST_SENDER);
	cr_assert_eq(memcmp(&message.payload, &payload, sizeof(payload)), 0);

	info = cap_lookup(info_cap);
	cr_assert_not_null(info);
	object = cap_object_acquire(info->cap_object_id);
	cr_assert_not_null(object);
	cap_request = (struct cap_request){
		.caller            = SIGNAL_TEST_SECOND_SENDER,
		.cap_id            = info_cap,
		.object_id         = signal_id(signal),
		.rights            = info->rights,
		.request           = &read_request,
		.request_size      = sizeof(read_request),
		.response          = &read_response,
		.response_capacity = sizeof(read_response),
	};
	result = object->handler(&cap_request);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(result.value, sizeof(read_response));
	cr_assert_eq(read_response.generation, 1u);
	cr_assert_eq(read_response.handler_count, 0u);
	cr_assert_eq(read_response.wait_subscription_count, 0u);
	cr_assert_eq(read_response.blocked_waiter_count, 0u);
	cr_assert_eq(read_response.caller_upcall_pending_count, 0u);
	cr_assert_eq(read_response.caller_upcall_dropped_count, 0u);
	cr_assert_eq(read_response.caller_upcall_capacity, USER_UPCALL_QUEUE_CAPACITY);
	cr_assert((read_response.flags & SIGNAL_READ_FLAG_HAS_VALUE) != 0u);
	cap_object_release(object);

	cr_assert_eq(signal_destroy(signal), SIGNAL_OK);
	cr_assert_null(cap_lookup(sender_cap));
	cr_assert_null(cap_lookup(reader_cap));
	cr_assert_null(cap_lookup(control_cap));
	cr_assert_null(cap_lookup(info_cap));
}

Test(signal, invalid_handler_flags_are_rejected) {
	struct signal* signal;
	struct uthread receiver;

	ipc_test_init_heap();
	signal = signal_create();
	cr_assert_not_null(signal);
	signal_test_init_handler_uthread(&receiver, 0x9000u);
	cr_assert_eq(signal_register_handler(signal, &receiver, signal_test_handler, 1u << 31), SIGNAL_INVALID_ARGUMENTS);
	cr_assert_eq(signal_handler_count(signal), 0u);

	cr_assert_eq(signal_destroy(signal), SIGNAL_OK);
	signal_test_deinit_uthread(&receiver);
}
