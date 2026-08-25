#pragma once

#include <base/upcall.h>
#include <base/vmm.h>
#include <core/spinlock.h>
#include <hal/userspace.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct hal_userspace_return_frame;
struct uthread;

enum {
	USER_UPCALL_QUEUE_CAPACITY = 32u,
};

/* Kernel-internal provenance used to identify and revoke queued requests. */
enum user_upcall_origin {
	USER_UPCALL_ORIGIN_NONE = 0,
	USER_UPCALL_ORIGIN_SIGNAL,
};

/* Queue-management properties attached to one pending request. */
enum user_upcall_flags {
	USER_UPCALL_FLAG_NONE          = 0u,
	USER_UPCALL_FLAG_NON_EVICTABLE = 1u << 0,
	USER_UPCALL_FLAG_COALESCIBLE   = 1u << 1,
};

/* One userspace entry, opaque arguments, and non-user-visible provenance. */
struct user_upcall_request {
	enum user_upcall_origin origin;
	uint32_t                flags;
	uintptr_t               origin_token;
	uint64_t                origin_id;
	uintptr_t               entry;
	uintptr_t               args[USER_UPCALL_ARGUMENT_COUNT];
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
	struct user_upcall_request*  pending;
	size_t                       head;
	size_t                       count;
	uint64_t                     dropped_count;
	size_t                       force_free_reservations;
	size_t                       force_eviction_reservations;
	enum user_upcall_origin      active_origin;
	uint64_t                     active_origin_id;
	enum user_upcall_phase       phase;
	bool                         initialized;
};

/* Initialize the state and allocate its fixed-capacity pending queue. */
bool uthread_upcall_state_init(struct uthread* thread);

/* Clear the state for one thread. */
void uthread_upcall_state_deinit(struct uthread* thread);

/* Queue one upcall request. */
enum user_upcall_result uthread_upcall_enqueue(struct uthread* thread, const struct user_upcall_request* request);

/*
 * Queue one coalescible request, replacing already-pending coalescible requests
 * with the same non-NONE origin and origin token. Non-coalescible requests are
 * never removed. Unrelated requests keep FIFO order and the replacement is
 * appended as the newest request.
 */
enum user_upcall_result uthread_upcall_enqueue_latest(struct uthread*                   thread,
                                                      const struct user_upcall_request* request);

/* Reserve admission for one forced request without changing the visible FIFO. */
enum user_upcall_result uthread_upcall_force_reserve(struct uthread* thread);

/* Cancel one outstanding forced-admission reservation. */
void uthread_upcall_force_cancel(struct uthread* thread);

/* Commit one previously reserved forced request. Reserved victims count as drops. */
enum user_upcall_result uthread_upcall_force_commit(struct uthread* thread, const struct user_upcall_request* request);

/* Force one request into a queue, evicting the oldest evictable request when full. */
enum user_upcall_result uthread_upcall_enqueue_force(struct uthread* thread, const struct user_upcall_request* request);

/* Remove queued requests matching one kernel origin and token. An active request is not affected. */
size_t uthread_upcall_purge(struct uthread* thread, enum user_upcall_origin origin, uintptr_t origin_token);

/* Deliver the next upcall, or consume one required resume boundary. */
enum user_upcall_result uthread_upcall_deliver(struct uthread* thread, struct hal_userspace_return_frame* frame);

/* Restore the interrupted context and require one return boundary before another delivery. */
enum user_upcall_result uthread_upcall_restore(struct uthread* thread, struct hal_userspace_return_frame* frame);

/* Return the number of queued requests. */
size_t uthread_upcall_pending_count(struct uthread* thread);

/* Return the saturating count of requests dropped because the queue was full. */
uint64_t uthread_upcall_dropped_count(struct uthread* thread);

/* Return whether one upcall is active. */
bool uthread_upcall_is_active(struct uthread* thread);
