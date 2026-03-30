#include <string.h>

#include "../thread_test.h"
#include "kheap_test.h"

#define KHEAP_THREAD_COUNT 4u
#define KHEAP_ALLOCS_PER_THREAD 64u
#define KHEAP_ALLOC_SIZE 128u

struct kheap_thread_ctx {
	struct test_barrier* barrier;
	uint8_t              fill;
	bool                 ok;
};

static void* kheap_worker(void* arg) {
	struct kheap_thread_ctx* ctx                           = (struct kheap_thread_ctx*)arg;
	void*                    ptrs[KHEAP_ALLOCS_PER_THREAD] = {0};

	test_barrier_wait(ctx->barrier);

	for (size_t i = 0; i < KHEAP_ALLOCS_PER_THREAD; i++) {
		ptrs[i] = kmalloc(KHEAP_ALLOC_SIZE);
		if (ptrs[i] == NULL) {
			ctx->ok = false;
			return NULL;
		}
		memset(ptrs[i], ctx->fill, KHEAP_ALLOC_SIZE);
	}

	for (size_t i = 0; i < KHEAP_ALLOCS_PER_THREAD; i++) {
		uint8_t* bytes = (uint8_t*)ptrs[i];

		for (size_t j = 0; j < KHEAP_ALLOC_SIZE; j++) {
			if (bytes[j] != ctx->fill) {
				ctx->ok = false;
				return NULL;
			}
		}

		kfree(ptrs[i]);
	}

	return NULL;
}

Test(kheap, concurrent_alloc_free_preserves_heap_space) {
	_Alignas(4096) static uint8_t arena[KiB(256)];
	struct test_barrier           barrier;
	pthread_t                     threads[KHEAP_THREAD_COUNT];
	struct kheap_thread_ctx       ctx[KHEAP_THREAD_COUNT];
	size_t                        initial_total;

	init_test_kheap(arena, sizeof(arena));
	initial_total = kheap_total_bytes();

	test_barrier_init(&barrier, KHEAP_THREAD_COUNT);

	for (size_t i = 0; i < KHEAP_THREAD_COUNT; i++) {
		ctx[i] = (struct kheap_thread_ctx){
			.barrier = &barrier,
			.fill    = (uint8_t)(0x40u + i),
			.ok      = true,
		};

		cr_assert_eq(pthread_create(&threads[i], NULL, kheap_worker, &ctx[i]), 0, "pthread_create failed");
	}

	for (size_t i = 0; i < KHEAP_THREAD_COUNT; i++) {
		cr_assert_eq(pthread_join(threads[i], NULL), 0, "pthread_join failed");
		cr_assert(ctx[i].ok, "kheap worker reported failure");
	}

	cr_assert_geq(kheap_total_bytes(), initial_total, "kheap capacity unexpectedly shrank");
	cr_assert_eq(
		kheap_free_bytes(), kheap_total_bytes(), "kheap did not fully recover free space after concurrent activity");
}
