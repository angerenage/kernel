#include <base/heap.h>
#include <core/pmm.h>
#include <core/user_upcall.h>
#include <core/uthread.h>
#include <criterion/criterion.h>
#include <hal/userspace.h>
#include <libc/string.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KiB(x) ((size_t)(x) * 1024u)
#define USER_UPCALL_TEST_HEAP_SIZE KiB(64)

static uint8_t user_upcall_test_heap[USER_UPCALL_TEST_HEAP_SIZE] __attribute__((aligned(PMM_PAGE_SIZE)));
static size_t  user_upcall_test_heap_offset;
static bool    user_upcall_test_heap_initialized;

bool heap_grow_pages(size_t page_count, void** out_base) {
	size_t bytes;
	size_t offset;

	if (out_base == NULL) return false;
	*out_base = NULL;

	bytes = page_count * PMM_PAGE_SIZE;
	for (;;) {
		offset = __atomic_load_n(&user_upcall_test_heap_offset, __ATOMIC_ACQUIRE);
		if (bytes > USER_UPCALL_TEST_HEAP_SIZE - offset) return false;
		if (__atomic_compare_exchange_n(
				&user_upcall_test_heap_offset, &offset, offset + bytes, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
			*out_base = user_upcall_test_heap + offset;
			return true;
		}
	}
}

static void user_upcall_test_init_heap(void) {
	if (user_upcall_test_heap_initialized) return;
	user_upcall_test_heap_offset = 0u;
	cr_assert(heap_init(), "heap_init failed");
	user_upcall_test_heap_initialized = true;
}

struct hal_userspace_return_frame {
	bool      user;
	uintptr_t entry;
	uintptr_t stack;
	uintptr_t args[USER_UPCALL_ARGUMENT_COUNT];
};

static void user_upcall_test_reset(struct uthread* thread, struct hal_userspace_return_frame* frame) {
	user_upcall_test_init_heap();
	memset(thread, 0, sizeof(*thread));
	cr_assert(uthread_upcall_state_init(thread), "uthread upcall state initialization failed");
	thread->process          = (struct process*)(uintptr_t)1u;
	thread->upcall.stack_id  = 1u;
	thread->upcall.stack_top = 0x9000u;
	*frame                   = (struct hal_userspace_return_frame){
						  .user  = true,
						  .entry = 0x1000u,
						  .stack = 0x8000u,
						  .args  = {0x11u, 0x22u, 0x33u, 0x44u, 0x55u},
    };
}

Test(user_upcall, validates_requests) {
	struct uthread                    thread;
	struct hal_userspace_return_frame frame;
	struct user_upcall_request        request = {
			   .entry = 0x4000u,
			   .args  = {1u, 2u, 3u, 4u, 5u},
    };

	user_upcall_test_reset(&thread, &frame);
	cr_assert_eq(uthread_upcall_enqueue(NULL, &request), USER_UPCALL_INVALID_ARGUMENTS);
	cr_assert_eq(uthread_upcall_enqueue(&thread, NULL), USER_UPCALL_INVALID_ARGUMENTS);
	request.entry = 0u;
	cr_assert_eq(uthread_upcall_enqueue(&thread, &request), USER_UPCALL_INVALID_ARGUMENTS);
	request.entry = 0x4000u;
	cr_assert_eq(uthread_upcall_enqueue(&thread, &request), USER_UPCALL_OK);
	cr_assert_eq(uthread_upcall_pending_count(&thread), 1u);
	uthread_upcall_state_deinit(&thread);
}

Test(user_upcall, rejects_dying_thread) {
	struct uthread                    thread;
	struct hal_userspace_return_frame frame;
	struct user_upcall_request        request = {
			   .entry = 0x4000u,
			   .args  = {1u, 2u, 3u, 4u},
    };

	user_upcall_test_reset(&thread, &frame);
	__atomic_store_n(&thread.dying, 1u, __ATOMIC_RELEASE);
	cr_assert_eq(uthread_upcall_enqueue(&thread, &request), USER_UPCALL_THREAD_DYING);
	uthread_upcall_state_deinit(&thread);
}

Test(user_upcall, delivers_fifo_and_restores_interrupted_context) {
	struct uthread                    thread;
	struct hal_userspace_return_frame frame;
	struct hal_userspace_return_frame original;
	struct user_upcall_request        request;

	user_upcall_test_reset(&thread, &frame);
	original = frame;
	for (size_t i = 0u; i < USER_UPCALL_QUEUE_CAPACITY; i++) {
		request = (struct user_upcall_request){
			.entry = 0x4000u + i * 0x1000u,
			.args  = {i + 1u, i + 101u, i + 201u, i + 301u, i + 401u},
		};
		cr_assert_eq(uthread_upcall_enqueue(&thread, &request), USER_UPCALL_OK);
	}
	cr_assert_eq(uthread_upcall_enqueue(&thread, &request), USER_UPCALL_QUEUE_FULL);

	cr_assert_eq(uthread_upcall_deliver(&thread, &frame), USER_UPCALL_OK);
	cr_assert_eq(frame.entry, 0x4000u);
	cr_assert_eq(frame.stack, 0x9000u);
	cr_assert_eq(frame.args[0], 1u);
	cr_assert_eq(frame.args[1], 101u);
	cr_assert_eq(frame.args[2], 201u);
	cr_assert_eq(frame.args[3], 301u);
	cr_assert_eq(frame.args[4], 401u);
	cr_assert(uthread_upcall_is_active(&thread));
	cr_assert_eq(uthread_upcall_pending_count(&thread), USER_UPCALL_QUEUE_CAPACITY - 1u);
	cr_assert_eq(uthread_upcall_deliver(&thread, &frame), USER_UPCALL_IDLE);

	cr_assert_eq(uthread_upcall_restore(&thread, &frame), USER_UPCALL_OK);
	cr_assert_eq(frame.user, original.user);
	cr_assert_eq(frame.entry, original.entry);
	cr_assert_eq(frame.stack, original.stack);
	cr_assert_eq(frame.args[0], original.args[0]);
	cr_assert_eq(frame.args[1], original.args[1]);
	cr_assert_eq(frame.args[2], original.args[2]);
	cr_assert_eq(frame.args[3], original.args[3]);
	cr_assert_eq(frame.args[4], original.args[4]);
	cr_assert_not(uthread_upcall_is_active(&thread));
	cr_assert_eq(thread.upcall.phase, USER_UPCALL_PHASE_RESUME);

	cr_assert_eq(uthread_upcall_deliver(&thread, &frame), USER_UPCALL_DEFERRED);
	cr_assert_eq(thread.upcall.phase, USER_UPCALL_PHASE_IDLE);
	cr_assert_eq(uthread_upcall_pending_count(&thread), USER_UPCALL_QUEUE_CAPACITY - 1u);
	cr_assert_eq(frame.entry, original.entry);

	cr_assert_eq(uthread_upcall_deliver(&thread, &frame), USER_UPCALL_OK);
	cr_assert_eq(frame.entry, 0x5000u);
	cr_assert_eq(frame.args[0], 2u);
	cr_assert_eq(frame.args[1], 102u);
	cr_assert_eq(frame.args[2], 202u);
	cr_assert_eq(frame.args[3], 302u);
	cr_assert_eq(frame.args[4], 402u);
	uthread_upcall_state_deinit(&thread);
}

Test(user_upcall, preserves_active_state_after_invalid_restore_frame) {
	struct uthread                    thread;
	struct hal_userspace_return_frame frame;
	struct user_upcall_request        request = {
			   .entry = 0x4000u,
			   .args  = {7u, 8u, 9u, 10u},
    };

	user_upcall_test_reset(&thread, &frame);
	cr_assert_eq(uthread_upcall_enqueue(&thread, &request), USER_UPCALL_OK);
	cr_assert_eq(uthread_upcall_deliver(&thread, &frame), USER_UPCALL_OK);

	frame.user = false;
	cr_assert_eq(uthread_upcall_restore(&thread, &frame), USER_UPCALL_CONTEXT_INVALID);
	cr_assert(uthread_upcall_is_active(&thread));
	frame.user = true;
	cr_assert_eq(uthread_upcall_restore(&thread, &frame), USER_UPCALL_OK);
	cr_assert_not(uthread_upcall_is_active(&thread));
	uthread_upcall_state_deinit(&thread);
}

Test(user_upcall, queues_requests_while_active) {
	struct uthread                    thread;
	struct hal_userspace_return_frame frame;
	struct user_upcall_request        first = {
			   .entry = 0x4000u,
			   .args  = {1u, 2u, 3u, 4u},
    };
	struct user_upcall_request second = {
		.entry = 0x5000u,
		.args  = {5u, 6u, 7u, 8u},
	};

	user_upcall_test_reset(&thread, &frame);
	cr_assert_eq(uthread_upcall_enqueue(&thread, &first), USER_UPCALL_OK);
	cr_assert_eq(uthread_upcall_deliver(&thread, &frame), USER_UPCALL_OK);
	cr_assert_eq(uthread_upcall_enqueue(&thread, &second), USER_UPCALL_OK);
	cr_assert_eq(uthread_upcall_deliver(&thread, &frame), USER_UPCALL_IDLE);
	cr_assert_eq(uthread_upcall_restore(&thread, &frame), USER_UPCALL_OK);
	cr_assert_eq(uthread_upcall_deliver(&thread, &frame), USER_UPCALL_DEFERRED);
	cr_assert_eq(uthread_upcall_pending_count(&thread), 1u);
	cr_assert_eq(uthread_upcall_deliver(&thread, &frame), USER_UPCALL_OK);
	cr_assert_eq(frame.entry, 0x5000u);
	cr_assert_eq(frame.args[3], 8u);
	uthread_upcall_state_deinit(&thread);
}

Test(user_upcall, defers_a_full_queue_after_restoring_userspace) {
	struct uthread                    thread;
	struct hal_userspace_return_frame frame;
	struct hal_userspace_return_frame original;
	struct user_upcall_request        request = {
			   .entry = 0x4000u,
			   .args  = {1u, 2u, 3u, 4u},
    };

	user_upcall_test_reset(&thread, &frame);
	original = frame;
	cr_assert_eq(uthread_upcall_enqueue(&thread, &request), USER_UPCALL_OK);
	cr_assert_eq(uthread_upcall_deliver(&thread, &frame), USER_UPCALL_OK);

	for (size_t i = 0u; i < USER_UPCALL_QUEUE_CAPACITY; i++) {
		request.entry   = 0x5000u + i * 0x1000u;
		request.args[0] = i;
		cr_assert_eq(uthread_upcall_enqueue(&thread, &request), USER_UPCALL_OK);
	}
	cr_assert_eq(uthread_upcall_pending_count(&thread), USER_UPCALL_QUEUE_CAPACITY);

	cr_assert_eq(uthread_upcall_restore(&thread, &frame), USER_UPCALL_OK);
	cr_assert_eq(thread.upcall.phase, USER_UPCALL_PHASE_RESUME);
	cr_assert_eq(uthread_upcall_deliver(&thread, &frame), USER_UPCALL_DEFERRED);
	cr_assert_eq(thread.upcall.phase, USER_UPCALL_PHASE_IDLE);
	cr_assert_eq(uthread_upcall_pending_count(&thread), USER_UPCALL_QUEUE_CAPACITY);
	cr_assert_eq(memcmp(&frame, &original, sizeof(frame)), 0);
	cr_assert_eq(uthread_upcall_enqueue(&thread, &request), USER_UPCALL_QUEUE_FULL);

	cr_assert_eq(uthread_upcall_deliver(&thread, &frame), USER_UPCALL_OK);
	cr_assert_eq(frame.entry, 0x5000u);
	cr_assert_eq(frame.args[0], 0u);
	uthread_upcall_state_deinit(&thread);
}

Test(user_upcall, purge_removes_only_matching_queued_requests_and_preserves_fifo) {
	struct uthread                    thread;
	struct hal_userspace_return_frame frame;
	const uintptr_t                   first_token  = 0x1111u;
	const uintptr_t                   second_token = 0x2222u;
	struct user_upcall_request        requests[]   = {
        {.origin = USER_UPCALL_ORIGIN_SIGNAL, .origin_token = first_token, .entry = 0x4000u, .args = {1u}},
        {.origin = USER_UPCALL_ORIGIN_SIGNAL, .origin_token = second_token, .entry = 0x5000u, .args = {2u}},
        {.origin = USER_UPCALL_ORIGIN_NONE, .entry = 0x6000u, .args = {3u}},
        {.origin = USER_UPCALL_ORIGIN_SIGNAL, .origin_token = first_token, .entry = 0x7000u, .args = {4u}},
        {.origin = USER_UPCALL_ORIGIN_SIGNAL, .origin_token = second_token, .entry = 0x8000u, .args = {5u}},
    };

	user_upcall_test_reset(&thread, &frame);
	for (size_t i = 0u; i < sizeof(requests) / sizeof(requests[0]); i++) {
		cr_assert_eq(uthread_upcall_enqueue(&thread, &requests[i]), USER_UPCALL_OK);
	}

	cr_assert_eq(uthread_upcall_purge(&thread, USER_UPCALL_ORIGIN_SIGNAL, first_token), 2u);
	cr_assert_eq(uthread_upcall_pending_count(&thread), 3u);
	cr_assert_eq(thread.upcall.pending[thread.upcall.head].args[0], 2u);
	cr_assert_eq(thread.upcall.pending[(thread.upcall.head + 1u) % USER_UPCALL_QUEUE_CAPACITY].args[0], 3u);
	cr_assert_eq(thread.upcall.pending[(thread.upcall.head + 2u) % USER_UPCALL_QUEUE_CAPACITY].args[0], 5u);

	cr_assert_eq(uthread_upcall_purge(&thread, USER_UPCALL_ORIGIN_NONE, first_token), 0u);
	cr_assert_eq(uthread_upcall_purge(&thread, USER_UPCALL_ORIGIN_SIGNAL, second_token), 2u);
	cr_assert_eq(uthread_upcall_pending_count(&thread), 1u);
	cr_assert_eq(thread.upcall.pending[thread.upcall.head].args[0], 3u);
	uthread_upcall_state_deinit(&thread);
}

Test(user_upcall, coalescing_preserves_latest_and_counts_only_real_drops) {
	struct uthread                    thread;
	struct hal_userspace_return_frame frame;
	const uintptr_t                   token   = 0x1111u;
	struct user_upcall_request        tracked = {
			   .origin       = USER_UPCALL_ORIGIN_SIGNAL,
			   .flags        = USER_UPCALL_FLAG_COALESCIBLE,
			   .origin_token = token,
			   .entry        = 0x4000u,
			   .args         = {1u, 2u, 3u, 4u, 5u},
    };
	struct user_upcall_request filler = {
		.entry = 0x5000u,
	};
	struct user_upcall_request unrelated = {
		.origin       = USER_UPCALL_ORIGIN_SIGNAL,
		.flags        = USER_UPCALL_FLAG_COALESCIBLE,
		.origin_token = 0x2222u,
		.entry        = 0x6000u,
	};
	size_t tail;

	user_upcall_test_reset(&thread, &frame);
	cr_assert_eq(uthread_upcall_enqueue_latest(&thread, &tracked), USER_UPCALL_OK);
	for (size_t i = 1u; i < USER_UPCALL_QUEUE_CAPACITY; i++) {
		filler.args[0] = i;
		cr_assert_eq(uthread_upcall_enqueue(&thread, &filler), USER_UPCALL_OK);
	}
	cr_assert_eq(uthread_upcall_pending_count(&thread), USER_UPCALL_QUEUE_CAPACITY);
	cr_assert_eq(uthread_upcall_dropped_count(&thread), 0u);

	cr_assert_eq(uthread_upcall_enqueue(&thread, &filler), USER_UPCALL_QUEUE_FULL);
	cr_assert_eq(uthread_upcall_dropped_count(&thread), 1u);

	tracked.entry   = 0x7000u;
	tracked.args[0] = 10u;
	tracked.args[4] = 50u;
	cr_assert_eq(uthread_upcall_enqueue_latest(&thread, &tracked), USER_UPCALL_OK);
	cr_assert_eq(uthread_upcall_pending_count(&thread), USER_UPCALL_QUEUE_CAPACITY);
	cr_assert_eq(uthread_upcall_dropped_count(&thread), 1u, "coalescing is intentional, not a dropped delivery");
	cr_assert_eq(thread.upcall.pending[thread.upcall.head].args[0], 1u, "unrelated FIFO order must be preserved");
	tail = (thread.upcall.head + thread.upcall.count - 1u) % USER_UPCALL_QUEUE_CAPACITY;
	cr_assert_eq(thread.upcall.pending[tail].entry, 0x7000u);
	cr_assert_eq(thread.upcall.pending[tail].args[0], 10u);
	cr_assert_eq(thread.upcall.pending[tail].args[4], 50u);

	cr_assert_eq(uthread_upcall_enqueue_latest(&thread, &unrelated), USER_UPCALL_QUEUE_FULL);
	cr_assert_eq(uthread_upcall_dropped_count(&thread), 2u);
	unrelated.origin = USER_UPCALL_ORIGIN_NONE;
	cr_assert_eq(uthread_upcall_enqueue_latest(&thread, &unrelated), USER_UPCALL_INVALID_ARGUMENTS);
	cr_assert_eq(uthread_upcall_dropped_count(&thread), 2u);

	thread.upcall.dropped_count = UINT64_MAX;
	cr_assert_eq(uthread_upcall_enqueue(&thread, &filler), USER_UPCALL_QUEUE_FULL);
	cr_assert_eq(uthread_upcall_dropped_count(&thread), UINT64_MAX, "drop count must saturate");
	uthread_upcall_state_deinit(&thread);
}

Test(user_upcall, coalescing_never_removes_a_normal_matching_request) {
	struct uthread                    thread;
	struct hal_userspace_return_frame frame;
	const uintptr_t                   token  = 0x1111u;
	struct user_upcall_request        queued = {
			   .origin       = USER_UPCALL_ORIGIN_SIGNAL,
			   .origin_token = token,
			   .entry        = 0x4000u,
			   .args         = {1u},
    };
	struct user_upcall_request coalesced = {
		.origin       = USER_UPCALL_ORIGIN_SIGNAL,
		.flags        = USER_UPCALL_FLAG_COALESCIBLE,
		.origin_token = token,
		.entry        = 0x5000u,
		.args         = {2u},
	};

	user_upcall_test_reset(&thread, &frame);
	cr_assert_eq(uthread_upcall_enqueue(&thread, &queued), USER_UPCALL_OK);
	cr_assert_eq(uthread_upcall_enqueue_latest(&thread, &coalesced), USER_UPCALL_OK);
	coalesced.entry   = 0x6000u;
	coalesced.args[0] = 3u;
	cr_assert_eq(uthread_upcall_enqueue_latest(&thread, &coalesced), USER_UPCALL_OK);

	cr_assert_eq(uthread_upcall_pending_count(&thread), 2u);
	cr_assert_eq(thread.upcall.pending[thread.upcall.head].entry, queued.entry);
	cr_assert_eq(thread.upcall.pending[thread.upcall.head].args[0], 1u);
	{
		size_t tail = (thread.upcall.head + 1u) % USER_UPCALL_QUEUE_CAPACITY;

		cr_assert_eq(thread.upcall.pending[tail].entry, 0x6000u);
		cr_assert_eq(thread.upcall.pending[tail].args[0], 3u);
		cr_assert((thread.upcall.pending[tail].flags & USER_UPCALL_FLAG_COALESCIBLE) != 0u);
	}
	cr_assert_eq(uthread_upcall_dropped_count(&thread), 0u);
	uthread_upcall_state_deinit(&thread);
}

Test(user_upcall, forced_enqueue_evicts_oldest_evictable_and_preserves_protected_requests) {
	struct uthread                    thread;
	struct hal_userspace_return_frame frame;
	struct user_upcall_request        protected = {
			   .flags = USER_UPCALL_FLAG_NON_EVICTABLE,
			   .entry = 0x4000u,
    };
	struct user_upcall_request evictable = {
		.entry = 0x5000u,
	};
	struct user_upcall_request forced = {
		.flags = USER_UPCALL_FLAG_NON_EVICTABLE,
		.entry = 0x6000u,
		.args  = {0xfeedu},
	};
	size_t tail;

	user_upcall_test_reset(&thread, &frame);
	protected.args[0] = 1u;
	cr_assert_eq(uthread_upcall_enqueue(&thread, &protected), USER_UPCALL_OK);
	evictable.args[0] = 2u;
	cr_assert_eq(uthread_upcall_enqueue(&thread, &evictable), USER_UPCALL_OK);
	for (size_t i = 2u; i < USER_UPCALL_QUEUE_CAPACITY; i++) {
		protected.args[0] = i + 1u;
		cr_assert_eq(uthread_upcall_enqueue(&thread, &protected), USER_UPCALL_OK);
	}

	cr_assert_eq(uthread_upcall_enqueue_force(&thread, &forced), USER_UPCALL_OK);
	cr_assert_eq(uthread_upcall_pending_count(&thread), USER_UPCALL_QUEUE_CAPACITY);
	cr_assert_eq(uthread_upcall_dropped_count(&thread), 1u);
	cr_assert_eq(thread.upcall.pending[thread.upcall.head].args[0], 1u);
	for (size_t i = 0u; i < thread.upcall.count; i++) {
		size_t index = (thread.upcall.head + i) % USER_UPCALL_QUEUE_CAPACITY;

		cr_assert_neq(thread.upcall.pending[index].args[0], 2u, "oldest evictable request must be removed");
	}
	tail = (thread.upcall.head + thread.upcall.count - 1u) % USER_UPCALL_QUEUE_CAPACITY;
	cr_assert_eq(thread.upcall.pending[tail].entry, forced.entry);
	cr_assert_eq(thread.upcall.pending[tail].args[0], 0xfeedu);
	cr_assert((thread.upcall.pending[tail].flags & USER_UPCALL_FLAG_NON_EVICTABLE) != 0u);

	cr_assert_eq(uthread_upcall_enqueue_force(&thread, &forced), USER_UPCALL_QUEUE_FULL);
	cr_assert_eq(uthread_upcall_dropped_count(&thread), 2u);
	uthread_upcall_state_deinit(&thread);
}

Test(user_upcall, forced_reservation_prevents_regular_enqueue_from_stealing_capacity) {
	struct uthread                    thread;
	struct hal_userspace_return_frame frame;
	struct user_upcall_request        regular = {
			   .entry = 0x4000u,
    };
	struct user_upcall_request forced = {
		.flags = USER_UPCALL_FLAG_NON_EVICTABLE,
		.entry = 0x5000u,
	};

	user_upcall_test_reset(&thread, &frame);
	for (size_t i = 0u; i + 1u < USER_UPCALL_QUEUE_CAPACITY; i++) {
		regular.args[0] = i;
		cr_assert_eq(uthread_upcall_enqueue(&thread, &regular), USER_UPCALL_OK);
	}
	cr_assert_eq(uthread_upcall_force_reserve(&thread), USER_UPCALL_OK);
	cr_assert_eq(uthread_upcall_enqueue(&thread, &regular), USER_UPCALL_QUEUE_FULL);
	cr_assert_eq(uthread_upcall_dropped_count(&thread), 1u);
	cr_assert_eq(uthread_upcall_force_commit(&thread, &forced), USER_UPCALL_OK);
	cr_assert_eq(uthread_upcall_pending_count(&thread), USER_UPCALL_QUEUE_CAPACITY);
	cr_assert_eq(thread.upcall.force_free_reservations, 0u);
	cr_assert_eq(thread.upcall.force_eviction_reservations, 0u);
	uthread_upcall_state_deinit(&thread);
}

Test(user_upcall, force_reserve_full_protected_queue_fails_without_recording_a_drop) {
	struct uthread                    thread;
	struct hal_userspace_return_frame frame;
	struct user_upcall_request        protected = {
			   .flags = USER_UPCALL_FLAG_NON_EVICTABLE,
			   .entry = 0x4000u,
    };

	user_upcall_test_reset(&thread, &frame);
	for (size_t i = 0u; i < USER_UPCALL_QUEUE_CAPACITY; i++) {
		protected.args[0] = i;
		cr_assert_eq(uthread_upcall_enqueue(&thread, &protected), USER_UPCALL_OK);
	}

	cr_assert_eq(uthread_upcall_force_reserve(&thread), USER_UPCALL_QUEUE_FULL);
	cr_assert_eq(thread.upcall.force_free_reservations, 0u);
	cr_assert_eq(thread.upcall.force_eviction_reservations, 0u);
	cr_assert_eq(uthread_upcall_dropped_count(&thread), 0u);
	cr_assert_eq(uthread_upcall_pending_count(&thread), USER_UPCALL_QUEUE_CAPACITY);
	uthread_upcall_state_deinit(&thread);
}

Test(user_upcall, canceling_eviction_reservation_restores_original_fifo) {
	struct uthread                    thread;
	struct hal_userspace_return_frame frame;
	struct user_upcall_request        request = {
			   .entry = 0x4000u,
    };

	user_upcall_test_reset(&thread, &frame);
	for (size_t i = 0u; i < USER_UPCALL_QUEUE_CAPACITY; i++) {
		request.args[0] = i + 1u;
		cr_assert_eq(uthread_upcall_enqueue(&thread, &request), USER_UPCALL_OK);
	}

	cr_assert_eq(uthread_upcall_force_reserve(&thread), USER_UPCALL_OK);
	cr_assert_eq(thread.upcall.force_free_reservations, 0u);
	cr_assert_eq(thread.upcall.force_eviction_reservations, 1u);
	cr_assert_eq(uthread_upcall_deliver(&thread, &frame),
	             USER_UPCALL_DEFERRED,
	             "the reserved victim must not be delivered while the reservation is live");

	uthread_upcall_force_cancel(&thread);
	cr_assert_eq(thread.upcall.force_free_reservations, 0u);
	cr_assert_eq(thread.upcall.force_eviction_reservations, 0u);
	cr_assert_eq(uthread_upcall_dropped_count(&thread), 0u);
	cr_assert_eq(uthread_upcall_deliver(&thread, &frame), USER_UPCALL_OK);
	cr_assert_eq(frame.entry, 0x4000u);
	cr_assert_eq(frame.args[0], 1u);
	cr_assert_eq(uthread_upcall_pending_count(&thread), USER_UPCALL_QUEUE_CAPACITY - 1u);
	uthread_upcall_state_deinit(&thread);
}

Test(user_upcall, purging_reserved_victim_rebalances_reservation_to_free_capacity) {
	struct uthread                    thread;
	struct hal_userspace_return_frame frame;
	const uintptr_t                   token  = 0x1111u;
	struct user_upcall_request        victim = {
			   .origin       = USER_UPCALL_ORIGIN_SIGNAL,
			   .origin_token = token,
			   .entry        = 0x4000u,
    };
	struct user_upcall_request protected = {
		.flags = USER_UPCALL_FLAG_NON_EVICTABLE,
		.entry = 0x5000u,
	};
	struct user_upcall_request forced = {
		.flags = USER_UPCALL_FLAG_NON_EVICTABLE,
		.entry = 0x6000u,
		.args  = {0xfeedu},
	};
	size_t tail;

	user_upcall_test_reset(&thread, &frame);
	cr_assert_eq(uthread_upcall_enqueue(&thread, &victim), USER_UPCALL_OK);
	for (size_t i = 1u; i < USER_UPCALL_QUEUE_CAPACITY; i++) {
		protected.args[0] = i;
		cr_assert_eq(uthread_upcall_enqueue(&thread, &protected), USER_UPCALL_OK);
	}

	cr_assert_eq(uthread_upcall_force_reserve(&thread), USER_UPCALL_OK);
	cr_assert_eq(thread.upcall.force_eviction_reservations, 1u);
	cr_assert_eq(uthread_upcall_purge(&thread, USER_UPCALL_ORIGIN_SIGNAL, token), 1u);
	cr_assert_eq(uthread_upcall_pending_count(&thread), USER_UPCALL_QUEUE_CAPACITY - 1u);
	cr_assert_eq(thread.upcall.force_eviction_reservations, 0u);
	cr_assert_eq(thread.upcall.force_free_reservations, 1u);
	cr_assert_eq(uthread_upcall_force_commit(&thread, &forced), USER_UPCALL_OK);
	cr_assert_eq(thread.upcall.force_free_reservations, 0u);
	cr_assert_eq(uthread_upcall_pending_count(&thread), USER_UPCALL_QUEUE_CAPACITY);
	cr_assert_eq(uthread_upcall_dropped_count(&thread), 0u);
	tail = (thread.upcall.head + thread.upcall.count - 1u) % USER_UPCALL_QUEUE_CAPACITY;
	cr_assert_eq(thread.upcall.pending[tail].entry, forced.entry);
	cr_assert_eq(thread.upcall.pending[tail].args[0], 0xfeedu);
	uthread_upcall_state_deinit(&thread);
}

Test(user_upcall, delivery_rebalances_eviction_reservation_to_free_capacity) {
	struct uthread                    thread;
	struct hal_userspace_return_frame frame;
	struct user_upcall_request        protected = {
			   .flags = USER_UPCALL_FLAG_NON_EVICTABLE,
			   .entry = 0x4000u,
    };
	struct user_upcall_request evictable = {
		.entry = 0x5000u,
		.args  = {0xdeadu},
	};
	struct user_upcall_request forced = {
		.flags = USER_UPCALL_FLAG_NON_EVICTABLE,
		.entry = 0x6000u,
		.args  = {0xfeedu},
	};
	size_t tail;

	user_upcall_test_reset(&thread, &frame);
	protected.args[0] = 1u;
	cr_assert_eq(uthread_upcall_enqueue(&thread, &protected), USER_UPCALL_OK);
	cr_assert_eq(uthread_upcall_enqueue(&thread, &evictable), USER_UPCALL_OK);
	for (size_t i = 2u; i < USER_UPCALL_QUEUE_CAPACITY; i++) {
		protected.args[0] = i + 1u;
		cr_assert_eq(uthread_upcall_enqueue(&thread, &protected), USER_UPCALL_OK);
	}

	cr_assert_eq(uthread_upcall_force_reserve(&thread), USER_UPCALL_OK);
	cr_assert_eq(thread.upcall.force_eviction_reservations, 1u);
	cr_assert_eq(uthread_upcall_deliver(&thread, &frame), USER_UPCALL_OK);
	cr_assert_eq(frame.entry, 0x4000u);
	cr_assert_eq(frame.args[0], 1u);
	cr_assert_eq(thread.upcall.force_eviction_reservations, 0u);
	cr_assert_eq(thread.upcall.force_free_reservations, 1u);
	cr_assert_eq(uthread_upcall_force_commit(&thread, &forced), USER_UPCALL_OK);
	cr_assert_eq(thread.upcall.force_free_reservations, 0u);
	cr_assert_eq(uthread_upcall_pending_count(&thread), USER_UPCALL_QUEUE_CAPACITY);
	cr_assert_eq(uthread_upcall_dropped_count(&thread), 0u);
	tail = (thread.upcall.head + thread.upcall.count - 1u) % USER_UPCALL_QUEUE_CAPACITY;
	cr_assert_eq(thread.upcall.pending[tail].entry, forced.entry);
	cr_assert_eq(thread.upcall.pending[tail].args[0], 0xfeedu);
	uthread_upcall_state_deinit(&thread);
}
