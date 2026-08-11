#include <core/ring_buffer.h>
#include <criterion/criterion.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "test_support.h"

Test(ring_buffer, init_rejects_capacity_element_size_overflow) {
	struct ring_buffer rb           = {0};
	const size_t       element_size = 16u;
	const size_t       capacity     = SIZE_MAX / element_size + 2u;

	ipc_test_init_heap();

	cr_assert_not(ring_buffer_init(&rb, "ring_buffer_overflow", SPINLOCK_ORDER_PROCESS, capacity, element_size),
	              "ring_buffer_init must reject capacity * element_size overflow");
}

Test(ring_buffer, wraparound_preserves_fifo_order_and_count) {
	struct ring_buffer rb         = {0};
	const uint32_t     initial[]  = {10u, 20u, 30u, 40u};
	const uint32_t     wrapped[]  = {50u, 60u};
	const uint32_t     expected[] = {30u, 40u, 50u, 60u};
	uint32_t           value      = 0u;

	ipc_test_init_heap();
	cr_assert(ring_buffer_init(&rb, "ring_buffer_wrap", SPINLOCK_ORDER_PROCESS, 4u, sizeof(uint32_t)));

	for (size_t i = 0u; i < 4u; i++) cr_assert(ring_buffer_enqueue(&rb, &initial[i]));
	cr_assert_eq(ring_buffer_count(&rb), 4u);

	cr_assert(ring_buffer_dequeue(&rb, &value));
	cr_assert_eq(value, initial[0]);
	cr_assert(ring_buffer_dequeue(&rb, &value));
	cr_assert_eq(value, initial[1]);
	cr_assert_eq(ring_buffer_count(&rb), 2u);

	for (size_t i = 0u; i < 2u; i++) cr_assert(ring_buffer_enqueue(&rb, &wrapped[i]));
	cr_assert_eq(ring_buffer_count(&rb), 4u);

	cr_assert(ring_buffer_peek(&rb, &value));
	cr_assert_eq(value, expected[0]);
	cr_assert_eq(ring_buffer_count(&rb), 4u, "peek must not consume the head element");

	for (size_t i = 0u; i < 4u; i++) {
		cr_assert(ring_buffer_dequeue(&rb, &value));
		cr_assert_eq(value, expected[i], "FIFO mismatch after wraparound at index %zu", i);
	}
	cr_assert_eq(ring_buffer_count(&rb), 0u);
	cr_assert_not(ring_buffer_dequeue(&rb, &value));

	ring_buffer_deinit(&rb);
}

Test(ring_buffer, failed_full_enqueue_does_not_change_existing_contents) {
	struct ring_buffer rb       = {0};
	const uint64_t     first    = 0x1111111111111111ull;
	const uint64_t     second   = 0x2222222222222222ull;
	const uint64_t     rejected = 0x3333333333333333ull;
	uint64_t           value    = 0u;

	ipc_test_init_heap();
	cr_assert(ring_buffer_init(&rb, "ring_buffer_full", SPINLOCK_ORDER_PROCESS, 2u, sizeof(uint64_t)));

	cr_assert(ring_buffer_enqueue(&rb, &first));
	cr_assert(ring_buffer_enqueue(&rb, &second));
	cr_assert_not(ring_buffer_enqueue(&rb, &rejected));
	cr_assert_eq(ring_buffer_count(&rb), 2u);

	cr_assert(ring_buffer_dequeue(&rb, &value));
	cr_assert_eq(value, first);
	cr_assert(ring_buffer_dequeue(&rb, &value));
	cr_assert_eq(value, second);
	cr_assert_not(ring_buffer_dequeue(&rb, &value));

	ring_buffer_deinit(&rb);
}

enum {
	RING_BUFFER_PRODUCER_COUNT      = 4u,
	RING_BUFFER_VALUES_PER_PRODUCER = 64u,
	RING_BUFFER_CONCURRENT_CAPACITY = RING_BUFFER_PRODUCER_COUNT * RING_BUFFER_VALUES_PER_PRODUCER,
};

struct producer_ctx {
	struct ring_buffer* rb;
	uint32_t            producer;
	bool                success;
};

static void* producer_entry(void* arg) {
	struct producer_ctx* ctx = arg;

	ctx->success = true;
	for (uint32_t i = 0u; i < RING_BUFFER_VALUES_PER_PRODUCER; i++) {
		const uint32_t value = ctx->producer * RING_BUFFER_VALUES_PER_PRODUCER + i;
		if (!ring_buffer_enqueue(ctx->rb, &value)) {
			ctx->success = false;
			break;
		}
	}
	return NULL;
}

Test(ring_buffer, concurrent_producers_publish_each_element_exactly_once) {
	struct ring_buffer  rb = {0};
	struct producer_ctx ctx[RING_BUFFER_PRODUCER_COUNT];
	pthread_t           threads[RING_BUFFER_PRODUCER_COUNT];
	bool                seen[RING_BUFFER_CONCURRENT_CAPACITY];

	ipc_test_init_heap();
	memset(seen, 0, sizeof(seen));
	cr_assert(ring_buffer_init(
		&rb, "ring_buffer_concurrent", SPINLOCK_ORDER_PROCESS, RING_BUFFER_CONCURRENT_CAPACITY, sizeof(uint32_t)));

	for (uint32_t i = 0u; i < RING_BUFFER_PRODUCER_COUNT; i++) {
		ctx[i] = (struct producer_ctx){.rb = &rb, .producer = i, .success = false};
		cr_assert_eq(pthread_create(&threads[i], NULL, producer_entry, &ctx[i]), 0);
	}
	for (uint32_t i = 0u; i < RING_BUFFER_PRODUCER_COUNT; i++) {
		cr_assert_eq(pthread_join(threads[i], NULL), 0);
		cr_assert(ctx[i].success, "producer %u unexpectedly observed a full queue", i);
	}

	cr_assert_eq(ring_buffer_count(&rb), RING_BUFFER_CONCURRENT_CAPACITY);
	for (size_t i = 0u; i < RING_BUFFER_CONCURRENT_CAPACITY; i++) {
		uint32_t value = UINT32_MAX;

		cr_assert(ring_buffer_dequeue(&rb, &value));
		cr_assert_lt(value, RING_BUFFER_CONCURRENT_CAPACITY);
		cr_assert_not(seen[value], "element %u was duplicated", value);
		seen[value] = true;
	}
	for (size_t i = 0u; i < RING_BUFFER_CONCURRENT_CAPACITY; i++) {
		cr_assert(seen[i], "element %zu was lost", i);
	}

	ring_buffer_deinit(&rb);
}
