#pragma once

#include <core/spinlock.h>
#include <hal/userspace.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct hal_userspace_return_frame;
struct uthread;

enum {
	USER_UPCALL_QUEUE_CAPACITY = 16u,
};

/* Three architecture-neutral machine-word arguments passed to the userspace entry point. */
struct user_upcall_event {
	uintptr_t args[3];
};

enum user_upcall_result {
	USER_UPCALL_OK = 0,
	USER_UPCALL_IDLE,
	USER_UPCALL_INVALID_ARGUMENTS,
	USER_UPCALL_NOT_CONFIGURED,
	USER_UPCALL_BUSY,
	USER_UPCALL_QUEUE_FULL,
	USER_UPCALL_THREAD_DYING,
	USER_UPCALL_NOT_ACTIVE,
	USER_UPCALL_CONTEXT_INVALID,
};

/* Per-uthread state. Callers must use the helpers below rather than mutating it directly. */
struct user_upcall_state {
	struct spinlock              lock;
	uintptr_t                    entry;
	uintptr_t                    stack_top;
	struct hal_userspace_context interrupted_context;
	struct user_upcall_event     pending[USER_UPCALL_QUEUE_CAPACITY];
	size_t                       head;
	size_t                       count;
	bool                         initialized;
	bool                         configured;
	bool                         active;
};

/* Lifecycle hooks owned by uthread initialization and destruction. */
void uthread_upcall_state_init(struct uthread* thread);
void uthread_upcall_state_deinit(struct uthread* thread);

/*
 * Configure the userspace entry point and exact stack top. Passing zero for both
 * disables delivery and discards queued events. Configuration cannot change
 * while an upcall is active.
 */
enum user_upcall_result uthread_upcall_configure(struct uthread* thread, uintptr_t entry, uintptr_t stack_top);

/* Queue one event for FIFO delivery. The target must already be configured. */
enum user_upcall_result uthread_upcall_enqueue(struct uthread* thread, const struct user_upcall_event* event);

/* Deliver the oldest pending event into frame, or return USER_UPCALL_IDLE when no delivery is required. */
enum user_upcall_result uthread_upcall_deliver(struct uthread* thread, struct hal_userspace_return_frame* frame);

/* Restore the context interrupted by the active upcall. */
enum user_upcall_result uthread_upcall_restore(struct uthread* thread, struct hal_userspace_return_frame* frame);

size_t uthread_upcall_pending_count(struct uthread* thread);
bool   uthread_upcall_is_active(struct uthread* thread);
