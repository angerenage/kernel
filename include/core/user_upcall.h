#pragma once

#include <base/vmm.h>
#include <core/spinlock.h>
#include <hal/userspace.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct hal_userspace_return_frame;
struct uthread;

enum {
	USER_UPCALL_ARGUMENT_COUNT = 4u,
	USER_UPCALL_QUEUE_CAPACITY = 16u,
};

/* One userspace entry and its opaque arguments. */
struct user_upcall_request {
	uintptr_t entry;
	uintptr_t args[USER_UPCALL_ARGUMENT_COUNT];
};

enum user_upcall_result {
	USER_UPCALL_OK = 0,
	USER_UPCALL_IDLE,
	USER_UPCALL_DEFERRED,
	USER_UPCALL_INVALID_ARGUMENTS,
	USER_UPCALL_QUEUE_FULL,
	USER_UPCALL_THREAD_DYING,
	USER_UPCALL_NOT_ACTIVE,
	USER_UPCALL_CONTEXT_INVALID,
};

/* Delivery lifecycle for one userspace thread. */
enum user_upcall_phase {
	/* No handler is running and the next queued request may be delivered. */
	USER_UPCALL_PHASE_IDLE = 0,
	/* Userspace is executing one handler with an interrupted context saved. */
	USER_UPCALL_PHASE_ACTIVE,
	/* The interrupted context was restored and must return once before another delivery. */
	USER_UPCALL_PHASE_RESUME,
};

/* State owned by one userspace thread. */
struct user_upcall_state {
	struct spinlock              lock;
	vmm_id_t                     stack_id;
	uintptr_t                    stack_top;
	struct hal_userspace_context interrupted_context;
	struct user_upcall_request   pending[USER_UPCALL_QUEUE_CAPACITY];
	size_t                       head;
	size_t                       count;
	enum user_upcall_phase       phase;
	bool                         initialized;
};

/* Initialize the state for one thread. */
void uthread_upcall_state_init(struct uthread* thread);

/* Clear the state for one thread. */
void uthread_upcall_state_deinit(struct uthread* thread);

/* Queue one upcall request. */
enum user_upcall_result uthread_upcall_enqueue(struct uthread* thread, const struct user_upcall_request* request);

/* Deliver the next upcall, or consume one required resume boundary. */
enum user_upcall_result uthread_upcall_deliver(struct uthread* thread, struct hal_userspace_return_frame* frame);

/* Restore the interrupted context and require one return boundary before another delivery. */
enum user_upcall_result uthread_upcall_restore(struct uthread* thread, struct hal_userspace_return_frame* frame);

/* Return the number of queued requests. */
size_t uthread_upcall_pending_count(struct uthread* thread);

/* Return whether one upcall is active. */
bool uthread_upcall_is_active(struct uthread* thread);
