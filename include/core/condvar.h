#pragma once

#include <core/mutex.h>
#include <core/thread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Sleepable condition variable backed by a scheduler wait queue. */
struct condvar {
	struct thread_wait_queue waiters;
};

/* Reset the condition variable to the unsignaled state with no waiters. */
void condvar_init(struct condvar* condvar);

/* Atomically release mutex, block until signaled, then reacquire mutex before returning. */
void condvar_wait(struct condvar* condvar, struct mutex* mutex);

/* Like condvar_wait, but returns false when timeout_ms elapses before a signal arrives. */
bool condvar_timed_wait(struct condvar* condvar, struct mutex* mutex, uint64_t timeout_ms);

/* Wake one blocked waiter, if present. */
bool condvar_signal(struct condvar* condvar);

/* Wake every blocked waiter and return the number made runnable. */
size_t condvar_broadcast(struct condvar* condvar);
