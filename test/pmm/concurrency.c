#include "../thread_test.h"
#include "pmm_test.h"

#define PMM_THREAD_COUNT 4u
#define PMM_BATCH_PAGES 2u

struct pmm_thread_ctx {
	struct test_barrier* barrier;
	uintptr_t*           slots;
	size_t               slot_count;
	bool                 ok;
};

static void* pmm_alloc_free_worker(void* arg) {
	struct pmm_thread_ctx* ctx = (struct pmm_thread_ctx*)arg;

	test_barrier_wait(ctx->barrier);

	for (size_t i = 0; i < ctx->slot_count; i++) {
		if (!pmm_alloc_pages(1, &ctx->slots[i])) {
			ctx->ok = false;
			return NULL;
		}
	}

	for (size_t i = 0; i < ctx->slot_count; i++) {
		if (!pmm_free_pages(ctx->slots[i], 1)) {
			ctx->ok = false;
			return NULL;
		}
	}

	return NULL;
}

Test(pmm, concurrent_alloc_free_round_trips_preserve_allocator_state) {
	_Alignas(4096) uint8_t arena[KiB(256)];
	struct test_barrier    barrier;
	pthread_t              threads[PMM_THREAD_COUNT];
	struct pmm_thread_ctx  ctx[PMM_THREAD_COUNT];
	uintptr_t              pages[PMM_THREAD_COUNT][PMM_BATCH_PAGES] = {{0}};
	size_t                 initial_free;

	init_test_pmm(arena, sizeof(arena));
	initial_free = pmm_free_page_count();

	cr_assert_geq(initial_free, PMM_THREAD_COUNT * PMM_BATCH_PAGES, "test arena does not have enough free pages");

	test_barrier_init(&barrier, PMM_THREAD_COUNT);

	for (size_t i = 0; i < PMM_THREAD_COUNT; i++) {
		ctx[i] = (struct pmm_thread_ctx){
			.barrier    = &barrier,
			.slots      = pages[i],
			.slot_count = PMM_BATCH_PAGES,
			.ok         = true,
		};

		cr_assert_eq(pthread_create(&threads[i], NULL, pmm_alloc_free_worker, &ctx[i]), 0, "pthread_create failed");
	}

	for (size_t i = 0; i < PMM_THREAD_COUNT; i++) {
		cr_assert_eq(pthread_join(threads[i], NULL), 0, "pthread_join failed");
		cr_assert(ctx[i].ok, "pmm worker reported failure");
	}

	cr_assert_eq(pmm_free_page_count(), initial_free, "pmm free page count changed after concurrent round-trips");
}
