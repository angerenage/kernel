#include <core/user_upcall.h>
#include <core/uthread.h>
#include <criterion/criterion.h>
#include <hal/userspace.h>
#include <libc/string.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct hal_userspace_return_frame {
	bool      user;
	uintptr_t entry;
	uintptr_t stack;
	uintptr_t args[4];
};

static void user_upcall_test_reset(struct uthread* thread, struct hal_userspace_return_frame* frame) {
	memset(thread, 0, sizeof(*thread));
	uthread_upcall_state_init(thread);
	thread->process          = (struct process*)(uintptr_t)1u;
	thread->upcall.stack_id  = 1u;
	thread->upcall.stack_top = 0x9000u;
	*frame                   = (struct hal_userspace_return_frame){
						  .user  = true,
						  .entry = 0x1000u,
						  .stack = 0x8000u,
						  .args  = {0x11u, 0x22u, 0x33u, 0x44u},
    };
}

Test(user_upcall, validates_requests) {
	struct uthread                    thread;
	struct hal_userspace_return_frame frame;
	struct user_upcall_request        request = {
			   .entry = 0x4000u,
			   .args  = {1u, 2u, 3u, 4u},
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
			.args  = {i + 1u, i + 101u, i + 201u, i + 301u},
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
