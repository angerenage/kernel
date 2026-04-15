#pragma once

#include <core/spinlock.h>
#include <core/thread.h>
#include <stdbool.h>

/* Non-recursive sleepable mutex layered on top of the core scheduler wait queues. */
struct mutex {
	struct spinlock          lock;
	struct thread*           owner;
	struct thread_wait_queue waiters;
};

/* Reset the mutex to the unlocked state with no current owner or waiters. */
void mutex_init(struct mutex* mutex);

/* Attempt to acquire the mutex once without blocking. */
bool mutex_try_lock(struct mutex* mutex);

/* Sleep until the mutex becomes available, then acquire it for the current thread. */
void mutex_lock(struct mutex* mutex);

/* Try to acquire the mutex before timeout_ms elapses. Returns false on timeout or invalid input. */
bool mutex_timed_lock(struct mutex* mutex, uint64_t timeout_ms);

/* Release the mutex. Returns false when the current thread is not the owner. */
bool mutex_unlock(struct mutex* mutex);

/* Return true while the mutex has a current owner. */
bool mutex_is_locked(const struct mutex* mutex);

/* Return true when the running thread currently owns the mutex. */
bool mutex_is_owned_by_current(const struct mutex* mutex);

/* Return the current owner, or NULL when the mutex is unlocked. */
struct thread* mutex_owner(const struct mutex* mutex);

/* Return the current number of blocked waiters. */
size_t mutex_waiter_count(struct mutex* mutex);
