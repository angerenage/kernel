#include <base/heap.h>
#include <core/cpu.h>
#include <core/pmm.h>
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

#define KiB(x) ((size_t)(x) * 1024u)
#define SIGNAL_TEST_HEAP_SIZE KiB(64)

static uint8_t signal_test_heap[SIGNAL_TEST_HEAP_SIZE] __attribute__((aligned(PMM_PAGE_SIZE)));
static size_t  signal_test_heap_offset;
static bool    signal_test_heap_initialized;

bool heap_grow_pages(size_t page_count, void** out_base) {
	size_t bytes;
	size_t offset;

	if (out_base == NULL) return false;
	*out_base = NULL;

	bytes = page_count * PMM_PAGE_SIZE;
	for (;;) {
		offset = __atomic_load_n(&signal_test_heap_offset, __ATOMIC_ACQUIRE);
		if (bytes > SIGNAL_TEST_HEAP_SIZE - offset) return false;
		if (__atomic_compare_exchange_n(
				&signal_test_heap_offset, &offset, offset + bytes, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
			*out_base = signal_test_heap + offset;
			return true;
		}
	}
}

static void signal_test_init_heap(void) {
	if (signal_test_heap_initialized) return;

	signal_test_heap_offset = 0u;
	cr_assert(heap_init(), "heap_init failed");
	signal_test_heap_initialized = true;
}

#define SIGNAL_TEST_SENDER ((process_id_t)42u)

static bool                  signal_test_hook_active;
static size_t                signal_test_hook_runs;
static struct signal*        signal_test_signal;
static struct thread*        signal_test_sender;
static struct signal_payload signal_test_first_payload;
static struct signal_payload signal_test_second_payload;
static bool                  signal_test_send_twice;
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
	signal_test_hook_active       = false;
	signal_test_hook_runs         = 0u;
	signal_test_signal            = NULL;
	signal_test_sender            = NULL;
	signal_test_first_payload     = (struct signal_payload){0};
	signal_test_second_payload    = (struct signal_payload){0};
	signal_test_send_twice        = false;
	signal_test_first_receivers   = 0u;
	signal_test_first_deliveries  = 0u;
	signal_test_second_receivers  = 0u;
	signal_test_second_deliveries = 0u;
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

	memset(target, 0, sizeof(*target));
	cr_assert(thread_init(&target->thread, &params), "uthread scheduler descriptor initialization failed");
	target->thread.owner_kind = THREAD_OWNER_UTHREAD;
	target->thread.owner      = target;
	target->process           = (struct process*)(uintptr_t)1u;
	target->reference_count   = 1u;
	uthread_upcall_state_init(target);
	target->upcall.stack_id  = 1u;
	target->upcall.stack_top = upcall_stack_top;
}

static void signal_test_init_handler_uthread(struct uthread* target, uintptr_t upcall_stack_top) {
	memset(target, 0, sizeof(*target));
	target->process         = (struct process*)(uintptr_t)1u;
	target->reference_count = 1u;
	uthread_upcall_state_init(target);
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
	cr_assert_eq(signal_send(signal_test_signal,
	                         SIGNAL_TEST_SENDER,
	                         &signal_test_first_payload,
	                         &signal_test_first_receivers,
	                         &signal_test_first_deliveries),
	             SIGNAL_OK);
	if (signal_test_send_twice) {
		cr_assert_eq(signal_send(signal_test_signal,
		                         SIGNAL_TEST_SENDER,
		                         &signal_test_second_payload,
		                         &signal_test_second_receivers,
		                         &signal_test_second_deliveries),
		             SIGNAL_OK);
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
	struct signal_payload received;
	uint64_t              receiver_count = UINT64_MAX;
	uint64_t              delivery_count = UINT64_MAX;
	signal_id_t           id;

	signal_test_init_heap();
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
	cr_assert_eq(memcmp(&payload, &received, sizeof(payload)), 0);

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

	signal_test_init_heap();
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
	struct signal_payload received;
	uint64_t              receiver_count;
	uint64_t              delivery_count;

	signal_test_init_heap();
	signal_test_init_bound_bootstrap_cpu();
	signal_test_init_sched_uthread(&first, "signal_first", 0x420000u, 0x424000u, 0x9000u);
	signal_test_init_sched_uthread(&second, "signal_second", 0x430000u, 0x434000u, 0xa000u);
	signal = signal_create();
	cr_assert_not_null(signal);

	cr_assert_eq(signal_send(signal, SIGNAL_TEST_SENDER, &payload, NULL, NULL), SIGNAL_OK);
	sched_set_current(cpu_current(), &first.thread);
	cr_assert_eq(signal_try_wait(signal, &received), SIGNAL_OK);
	cr_assert_eq(memcmp(&payload, &received, sizeof(payload)), 0);
	cr_assert_eq(signal_try_wait(signal, &received), SIGNAL_WOULD_BLOCK);

	sched_set_current(cpu_current(), &second.thread);
	cr_assert_eq(signal_try_wait(signal, &received), SIGNAL_OK);
	cr_assert_eq(memcmp(&payload, &received, sizeof(payload)), 0);
	cr_assert_eq(signal_wait_subscription_count(signal), 2u);

	payload.args[0] = 99u;
	cr_assert_eq(signal_send(signal, SIGNAL_TEST_SENDER, &payload, &receiver_count, &delivery_count), SIGNAL_OK);
	cr_assert_eq(receiver_count, 0u, "inactive synchronous cursors are not immediate receivers");
	cr_assert_eq(delivery_count, 0u);

	sched_set_current(cpu_current(), &first.thread);
	cr_assert_eq(signal_try_wait(signal, &received), SIGNAL_OK);
	cr_assert_eq(received.args[0], 99u);
	sched_set_current(cpu_current(), &second.thread);
	cr_assert_eq(signal_try_wait(signal, &received), SIGNAL_OK);
	cr_assert_eq(received.args[0], 99u);
	cr_assert_eq(signal_read(signal, &received), SIGNAL_OK);
	cr_assert_eq(received.args[0], 99u);
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

	signal_test_init_heap();
	signal = signal_create();
	cr_assert_not_null(signal);
	signal_test_init_handler_uthread(&first, 0x9000u);
	signal_test_init_handler_uthread(&second, 0xa000u);
	cr_assert_eq(signal_register_handler(signal, &first, signal_test_handler), SIGNAL_OK);
	cr_assert_eq(signal_register_handler(signal, &second, signal_test_handler), SIGNAL_OK);

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
	struct signal_payload received;

	signal_test_init_heap();
	signal_test_init_bound_bootstrap_cpu();
	signal_test_init_sched_uthread(&receiver, "signal_receiver", 0x450000u, 0x454000u, 0x9000u);
	cr_assert(thread_init(&sender, &sender_params), "sender thread_init failed");
	cr_assert(sched_make_runnable(&sender), "sender should become runnable");

	signal = signal_create();
	cr_assert_not_null(signal);
	cr_assert_eq(signal_register_handler(signal, &receiver, signal_test_handler), SIGNAL_OK);
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
	cr_assert_eq(memcmp(&received, &signal_test_first_payload, sizeof(received)), 0);
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
	struct signal_payload received;

	signal_test_init_heap();
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
	cr_assert_eq(received.args[0], 1u, "the blocked wait must receive the value that woke it");
	cr_assert_eq(signal_test_first_receivers, 1u);
	cr_assert_eq(signal_test_first_deliveries, 1u);
	cr_assert_eq(signal_test_second_receivers, 0u);
	cr_assert_eq(signal_test_second_deliveries, 0u);
	cr_assert_eq(signal_try_wait(signal, &received), SIGNAL_OK);
	cr_assert_eq(received.args[0], 5u, "the next wait must observe the newer remembered value");
	cr_assert_eq(signal_try_wait(signal, &received), SIGNAL_WOULD_BLOCK);

	cr_assert_eq(signal_destroy(signal), SIGNAL_OK);
	signal_test_deinit_uthread(&receiver);
	signal_test_reset_scheduler_state();
}

Test(signal, thread_cleanup_removes_handlers_and_synchronous_cursors) {
	struct signal*        first;
	struct signal*        second;
	struct uthread        receiver;
	struct signal_payload unused;

	signal_test_init_heap();
	signal_test_init_bound_bootstrap_cpu();
	signal_test_init_sched_uthread(&receiver, "signal_receiver", 0x480000u, 0x484000u, 0x9000u);
	first  = signal_create();
	second = signal_create();
	cr_assert_not_null(first);
	cr_assert_not_null(second);

	cr_assert_eq(signal_register_handler(first, &receiver, signal_test_handler), SIGNAL_OK);
	cr_assert_eq(signal_register_handler(second, &receiver, signal_test_handler), SIGNAL_OK);
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
