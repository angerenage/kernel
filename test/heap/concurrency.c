#include <string.h>

#include "../thread_test.h"
#include "heap_test.h"

#define HEAP_THREAD_COUNT 4u
#define HEAP_ALLOCS_PER_THREAD 64u
#define HEAP_ALLOC_SIZE 128u

struct heap_thread_ctx {
	struct test_barrier* barrier;
	uint8_t              fill;
	bool                 ok;
};

static void* heap_worker(void* arg) {
	struct heap_thread_ctx* ctx                          = (struct heap_thread_ctx*)arg;
	void*                   ptrs[HEAP_ALLOCS_PER_THREAD] = {0};

	test_barrier_wait(ctx->barrier);

	for (size_t i = 0; i < HEAP_ALLOCS_PER_THREAD; i++) {
		ptrs[i] = malloc(HEAP_ALLOC_SIZE);
		if (ptrs[i] == NULL) {
			ctx->ok = false;
			return NULL;
		}
		memset(ptrs[i], ctx->fill, HEAP_ALLOC_SIZE);
	}

	for (size_t i = 0; i < HEAP_ALLOCS_PER_THREAD; i++) {
		uint8_t* bytes = (uint8_t*)ptrs[i];

		for (size_t j = 0; j < HEAP_ALLOC_SIZE; j++) {
			if (bytes[j] != ctx->fill) {
				ctx->ok = false;
				return NULL;
			}
		}

		free(ptrs[i]);
	}

	return NULL;
}

Test(heap, concurrent_alloc_free_preserves_heap_space) {
	_Alignas(4096) static uint8_t arena[KiB(256)];
	struct test_barrier           barrier;
	pthread_t                     threads[HEAP_THREAD_COUNT];
	struct heap_thread_ctx        ctx[HEAP_THREAD_COUNT];
	size_t                        initial_total;

	init_test_heap(arena, sizeof(arena));
	initial_total = heap_total_bytes();

	test_barrier_init(&barrier, HEAP_THREAD_COUNT);

	for (size_t i = 0; i < HEAP_THREAD_COUNT; i++) {
		ctx[i] = (struct heap_thread_ctx){
			.barrier = &barrier,
			.fill    = (uint8_t)(0x40u + i),
			.ok      = true,
		};

		cr_assert_eq(pthread_create(&threads[i], NULL, heap_worker, &ctx[i]), 0, "pthread_create failed");
	}

	for (size_t i = 0; i < HEAP_THREAD_COUNT; i++) {
		cr_assert_eq(pthread_join(threads[i], NULL), 0, "pthread_join failed");
		cr_assert(ctx[i].ok, "heap worker reported failure");
	}

	cr_assert_geq(heap_total_bytes(), initial_total, "heap capacity unexpectedly shrank");
	cr_assert_eq(
		heap_free_bytes(), heap_total_bytes(), "heap did not fully recover free space after concurrent activity");
}
