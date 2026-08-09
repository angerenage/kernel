#include "test_support.h"

Test(thread, run_queue_orders_priorities_and_preserves_fifo_ties) {
	struct run_queue queue;
	struct thread    low;
	struct thread    first_high;
	struct thread    normal;
	struct thread    second_high;

	irq_enable_local();
	run_queue_init(&queue);
	thread_regression_init(&low, "low", 0x800000u, THREAD_PRIORITY_DEFAULT - 4);
	thread_regression_init(&first_high, "first_high", 0x804000u, THREAD_PRIORITY_DEFAULT + 4);
	thread_regression_init(&normal, "normal", 0x808000u, THREAD_PRIORITY_DEFAULT);
	thread_regression_init(&second_high, "second_high", 0x80c000u, THREAD_PRIORITY_DEFAULT + 4);

	cr_assert_not(run_queue_enqueue(NULL, &low), "NULL queue argument must be rejected");
	cr_assert(run_queue_enqueue(&queue, &low), "low enqueue failed");
	cr_assert(run_queue_enqueue(&queue, &first_high), "first high enqueue failed");
	cr_assert(run_queue_enqueue(&queue, &normal), "normal enqueue failed");
	cr_assert(run_queue_enqueue(&queue, &second_high), "second high enqueue failed");
	cr_assert_eq(run_queue_depth(&queue), 4u, "run queue depth mismatch");

	cr_assert_eq(run_queue_dequeue(&queue), &first_high, "first high-priority thread must lead");
	cr_assert_eq(run_queue_dequeue(&queue), &second_high, "equal-priority peers must remain FIFO");
	cr_assert_eq(run_queue_dequeue(&queue), &normal, "normal-priority thread order mismatch");
	cr_assert_eq(run_queue_dequeue(&queue), &low, "low-priority thread must remain last");
	cr_assert_null(run_queue_dequeue(&queue), "empty queue must dequeue NULL");
	cr_assert_eq(run_queue_depth(&queue), 0u, "empty queue depth must be zero");

	thread_regression_reset();
}

Test(thread, run_queue_duplicate_enqueue_is_side_effect_free_and_reusable_after_dequeue) {
	struct run_queue queue;
	struct thread    thread;

	irq_enable_local();
	run_queue_init(&queue);
	thread_regression_init(&thread, "worker", 0x810000u, THREAD_PRIORITY_DEFAULT);

	cr_assert(run_queue_enqueue(&queue, &thread), "initial enqueue failed");
	cr_assert(thread_is_queued(&thread), "enqueued thread must expose queued state");
	cr_assert_eq(run_queue_depth(&queue), 1u, "initial depth mismatch");

	cr_assert_not(run_queue_enqueue(&queue, &thread), "duplicate enqueue must be rejected");
	cr_assert_eq(run_queue_depth(&queue), 1u, "duplicate enqueue must not alter depth");
	cr_assert(thread_is_queued(&thread), "duplicate enqueue must not clear queued state");

	cr_assert_eq(run_queue_dequeue(&queue), &thread, "dequeue returned wrong thread");
	cr_assert_not(thread_is_queued(&thread), "dequeue must clear queued state");
	cr_assert_eq(run_queue_depth(&queue), 0u, "dequeue must consume exactly one entry");

	cr_assert(run_queue_enqueue(&queue, &thread), "dequeued thread must be reusable");
	cr_assert_eq(run_queue_dequeue(&queue), &thread, "re-enqueued thread mismatch");
	cr_assert_eq(run_queue_depth(&queue), 0u, "queue must end empty");

	thread_regression_reset();
}

Test(thread, run_queue_requeue_tracks_priority_changes_without_duplication) {
	struct run_queue queue;
	struct thread    first;
	struct thread    second;
	struct thread    third;

	irq_enable_local();
	run_queue_init(&queue);
	thread_regression_init(&first, "first", 0x820000u, THREAD_PRIORITY_DEFAULT);
	thread_regression_init(&second, "second", 0x824000u, THREAD_PRIORITY_DEFAULT);
	thread_regression_init(&third, "third", 0x828000u, THREAD_PRIORITY_DEFAULT);

	cr_assert(run_queue_enqueue(&queue, &first), "first enqueue failed");
	cr_assert(run_queue_enqueue(&queue, &second), "second enqueue failed");
	cr_assert(run_queue_enqueue(&queue, &third), "third enqueue failed");

	second.effective_priority = THREAD_PRIORITY_DEFAULT + 5;
	cr_assert(run_queue_requeue(&queue, &second), "priority boost requeue failed");
	cr_assert_eq(run_queue_depth(&queue), 3u, "requeue must not change depth");
	cr_assert_eq(run_queue_dequeue(&queue), &second, "boosted thread must move to the front");

	second.effective_priority = THREAD_PRIORITY_DEFAULT;
	cr_assert(run_queue_enqueue(&queue, &second), "second re-enqueue failed");
	cr_assert_eq(run_queue_dequeue(&queue), &first, "original FIFO head must remain first");
	cr_assert_eq(run_queue_dequeue(&queue), &third, "third must remain ahead of reinserted peer");
	cr_assert_eq(run_queue_dequeue(&queue), &second, "reinserted equal-priority thread must join the tail");
	cr_assert_eq(run_queue_depth(&queue), 0u, "queue must end empty");

	thread_regression_reset();
}

Test(thread, terminated_and_idle_threads_cannot_enter_run_queue) {
	struct run_queue queue;
	struct thread    exiting;
	struct thread    zombie;
	struct thread    idle;

	irq_enable_local();
	run_queue_init(&queue);
	thread_regression_init(&exiting, "exiting", 0x850000u, THREAD_PRIORITY_DEFAULT);
	thread_regression_init(&zombie, "zombie", 0x854000u, THREAD_PRIORITY_DEFAULT);
	thread_mark_exiting(&exiting, 1u);
	thread_mark_zombie(&zombie);
	thread_init_idle(&idle, NULL, "idle/test");

	cr_assert_not(run_queue_enqueue(&queue, &exiting), "EXITING thread must be rejected");
	cr_assert_not(run_queue_enqueue(&queue, &zombie), "ZOMBIE thread must be rejected");
	cr_assert_not(run_queue_enqueue(&queue, &idle), "idle thread must be rejected");
	cr_assert_eq(run_queue_depth(&queue), 0u, "rejected threads must not alter queue depth");

	thread_regression_reset();
}
