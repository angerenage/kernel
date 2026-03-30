#include <core/pmm.h>

#include "../thread_test.h"
#include "vaddr_alloc_test.h"

#define VADDR_THREAD_COUNT 4u
#define VADDR_ALLOCS_PER_THREAD 8u

struct vaddr_thread_ctx {
	struct test_barrier* barrier;
	uintptr_t*           addrs;
	bool                 ok;
};

static void* vaddr_worker(void* arg) {
	struct vaddr_thread_ctx* ctx = (struct vaddr_thread_ctx*)arg;

	test_barrier_wait(ctx->barrier);

	for (size_t i = 0; i < VADDR_ALLOCS_PER_THREAD; i++) {
		if (!vaddr_alloc_reserve(1, 1, &ctx->addrs[i])) {
			ctx->ok = false;
			return NULL;
		}
	}

	for (size_t i = 0; i < VADDR_ALLOCS_PER_THREAD; i++) {
		if (!vaddr_alloc_release(ctx->addrs[i], 1)) {
			ctx->ok = false;
			return NULL;
		}
	}

	return NULL;
}

Test(vaddr_alloc, concurrent_reserve_release_preserves_free_space) {
	_Alignas(4096) uint8_t  arena[KiB(256)];
	struct test_barrier     barrier;
	pthread_t               threads[VADDR_THREAD_COUNT];
	struct vaddr_thread_ctx ctx[VADDR_THREAD_COUNT];
	uintptr_t               addrs[VADDR_THREAD_COUNT][VADDR_ALLOCS_PER_THREAD] = {{0}};
	size_t                  initial_free;

	init_test_vaddr_alloc(arena, sizeof(arena), 0x30000000ull, 128);
	initial_free = vaddr_alloc_free_page_count();

	test_barrier_init(&barrier, VADDR_THREAD_COUNT);

	for (size_t i = 0; i < VADDR_THREAD_COUNT; i++) {
		ctx[i] = (struct vaddr_thread_ctx){
			.barrier = &barrier,
			.addrs   = addrs[i],
			.ok      = true,
		};

		cr_assert_eq(pthread_create(&threads[i], NULL, vaddr_worker, &ctx[i]), 0, "pthread_create failed");
	}

	for (size_t i = 0; i < VADDR_THREAD_COUNT; i++) {
		cr_assert_eq(pthread_join(threads[i], NULL), 0, "pthread_join failed");
		cr_assert(ctx[i].ok, "vaddr worker reported failure");
	}

	cr_assert_eq(vaddr_alloc_free_page_count(), initial_free, "vaddr allocator leaked pages under concurrency");
}
