#pragma once

#include <core/spinlock.h>
#include <core/thread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Sleepable reader-writer lock with writer preference once a writer queues. */
struct rwlock {
	struct spinlock          lock;
	size_t                   reader_count;
	size_t                   waiting_writers;
	struct thread*           writer;
	struct thread_wait_queue readers;
	struct thread_wait_queue writers;
};

/* Reset the rwlock to the unlocked state with no active owner or waiters. */
void rwlock_init(struct rwlock* rwlock);

/* Attempt to acquire a shared read lock without blocking. */
bool rwlock_try_read_lock(struct rwlock* rwlock);

/* Sleep until a shared read lock becomes available. */
void rwlock_read_lock(struct rwlock* rwlock);

/* Try to acquire a shared read lock before timeout_ms elapses. */
bool rwlock_timed_read_lock(struct rwlock* rwlock, uint64_t timeout_ms);

/* Attempt to acquire the exclusive write lock without blocking. */
bool rwlock_try_write_lock(struct rwlock* rwlock);

/* Sleep until the exclusive write lock becomes available. */
void rwlock_write_lock(struct rwlock* rwlock);

/* Try to acquire the exclusive write lock before timeout_ms elapses. */
bool rwlock_timed_write_lock(struct rwlock* rwlock, uint64_t timeout_ms);

/* Release one shared reader hold. */
bool rwlock_read_unlock(struct rwlock* rwlock);

/* Release the exclusive writer hold. */
bool rwlock_write_unlock(struct rwlock* rwlock);

/* Return the current number of active readers. */
size_t rwlock_reader_count(const struct rwlock* rwlock);

/* Return the current number of blocked readers plus blocked writers. */
size_t rwlock_waiter_count(struct rwlock* rwlock);
