#pragma once

#include <core/spinlock.h>
#include <core/thread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Sleepable counting semaphore layered on top of the core scheduler wait queues. */
struct semaphore {
	struct spinlock          lock;
	size_t                   count;
	struct thread_wait_queue waiters;
};

/* Reset the semaphore to initial_count permits with no blocked waiters. */
void semaphore_init(struct semaphore* semaphore, size_t initial_count);

/* Attempt to consume one permit without blocking. */
bool semaphore_try_acquire(struct semaphore* semaphore);

/* Sleep until one permit becomes available, then consume it. */
void semaphore_acquire(struct semaphore* semaphore);

/* Try to consume one permit before timeout_ms elapses. Returns false on timeout or invalid input. */
bool semaphore_timed_acquire(struct semaphore* semaphore, uint64_t timeout_ms);

/* Publish one permit and wake one blocked waiter, if present. Returns false on overflow or invalid input. */
bool semaphore_release(struct semaphore* semaphore);

/* Return the current number of available permits. */
size_t semaphore_count(const struct semaphore* semaphore);

/* Return the current number of blocked waiters. */
size_t semaphore_waiter_count(struct semaphore* semaphore);
