#pragma once

#include <core/thread.h>
#include <stdbool.h>

/*
 * Higher-level kernel-thread helpers layered on top of core thread and
 * scheduler primitives.
 */

/* Return the thread descriptor currently associated with the running CPU. */
struct thread* kthread_current(void);

/* Initialize a kernel-thread descriptor using core thread create parameters. */
bool kthread_create(struct thread* thread, const struct thread_create_params* params);

/* Queue a kernel thread so it becomes runnable by the scheduler. */
bool kthread_start(struct thread* thread);

/* Yield the current thread's CPU slot to the next runnable thread. */
void kthread_yield(void);

/* Block until target exits (or is already exited), then publish its exit code. */
bool kthread_join(struct thread* target, thread_exit_code_t* out_exit_code);

/* Mark target as detached so no thread may join it. */
bool kthread_detach(struct thread* target);

/* Request deferred cancellation on target thread. */
bool kthread_cancel(struct thread* target);

/* Publish the current thread exit code and hand control back to the scheduler. */
__attribute__((noreturn))
void kthread_exit(thread_exit_code_t exit_code);
