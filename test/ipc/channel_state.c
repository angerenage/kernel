#include "test_support.h"

#define CHANNEL_STATE_THREAD_COUNT 8u

struct channel_state_thread_ctx {
	struct test_barrier*          barrier;
	struct process_channel_state* state;
	struct channel*               channel;
	bool                          added;
};

static void* channel_state_worker(void* arg) {
	struct channel_state_thread_ctx* ctx = arg;

	test_barrier_wait(ctx->barrier);
	ctx->added = process_channel_state_add(ctx->state, ctx->channel);
	test_barrier_wait(ctx->barrier);
	test_barrier_wait(ctx->barrier);
	process_channel_state_remove(ctx->state, ctx->channel);
	test_barrier_wait(ctx->barrier);
	return NULL;
}

Test(channel, process_channel_state_is_safe_for_concurrent_threads) {
	struct process_channel_state    state;
	struct test_barrier             barrier;
	struct channel*                 channels[CHANNEL_STATE_THREAD_COUNT];
	struct channel_state_thread_ctx contexts[CHANNEL_STATE_THREAD_COUNT];
	pthread_t                       threads[CHANNEL_STATE_THREAD_COUNT];

	ipc_test_init_heap();
	process_channel_state_init(&state);
	test_barrier_init(&barrier, CHANNEL_STATE_THREAD_COUNT + 1u);

	for (size_t i = 0u; i < CHANNEL_STATE_THREAD_COUNT; i++) {
		channels[i] = channel_create(1u);
		cr_assert_not_null(channels[i]);
		contexts[i] = (struct channel_state_thread_ctx){
			.barrier = &barrier,
			.state   = &state,
			.channel = channels[i],
			.added   = false,
		};
		cr_assert_eq(pthread_create(&threads[i], NULL, channel_state_worker, &contexts[i]), 0, "pthread_create failed");
	}

	test_barrier_wait(&barrier);
	test_barrier_wait(&barrier);
	cr_assert_eq(state.count, CHANNEL_STATE_THREAD_COUNT);
	for (size_t i = 0u; i < CHANNEL_STATE_THREAD_COUNT; i++) {
		bool found = false;

		cr_assert(contexts[i].added, "channel state add failed for worker %zu", i);
		for (size_t slot = 0u; slot < CHANNEL_MAX_PER_PROCESS; slot++) {
			if (state.channels[slot] == channels[i]) {
				found = true;
				break;
			}
		}
		cr_assert(found, "channel %zu was lost during concurrent registration", i);
	}

	test_barrier_wait(&barrier);
	test_barrier_wait(&barrier);
	for (size_t i = 0u; i < CHANNEL_STATE_THREAD_COUNT; i++) {
		cr_assert_eq(pthread_join(threads[i], NULL), 0, "pthread_join failed");
	}
	cr_assert_eq(state.count, 0u);

	for (size_t i = 0u; i < CHANNEL_STATE_THREAD_COUNT; i++) {
		cr_assert_eq(channel_destroy(channels[i], 1u), CHANNEL_OK);
	}
}
