#pragma once

#include <core/thread.h>
#include <stdbool.h>
#include <stddef.h>

struct cpu;

/*
 * Minimal per-CPU scheduler front-end.
 * Each CPU owns a run queue plus a permanently available idle thread.
 */

/* Create per-CPU scheduler state and one idle thread for every CPU in the topology. */
bool sched_init(void);

/* Attach the target CPU to its idle thread so the scheduler has a running thread baseline. */
bool sched_start_cpu(struct cpu* cpu);

/* Force the scheduler's current-thread pointer for a CPU and mark that thread running. */
void sched_set_current(struct cpu* cpu, struct thread* thread);

/* Return the thread currently associated with the running CPU. */
struct thread* sched_current_thread(void);

/* Return the always-present idle thread for a CPU. */
struct thread* sched_idle_thread(struct cpu* cpu);

/* Queue a runnable thread onto its preferred CPU, or the scheduler's default target when none is recorded. */
bool sched_make_runnable(struct thread* thread);

/* Remove a thread from its current CPU run queue before it begins running. */
bool sched_remove_runnable(struct thread* thread);

/* Yield the current CPU so another runnable thread may be dispatched. */
void sched_yield(void);

/* Block the current thread on queue until another CPU or event source wakes it. */
void sched_block_current(struct thread_wait_queue* queue, enum thread_block_reason reason);

/* Wake one blocked waiter from queue and make it runnable again. */
bool sched_wake_one(struct thread_wait_queue* queue);

/* Wake every blocked waiter from queue and return the number of threads made runnable. */
size_t sched_wake_all(struct thread_wait_queue* queue);

/* Publish the current thread's final exit status, wake joiners, and never return. */
void sched_exit_current(thread_exit_code_t exit_code) __attribute__((noreturn));

/* Return the current run-queue depth for a CPU. */
size_t sched_run_queue_depth(struct cpu* cpu);

/* Enter the CPU's idle loop and never return. */
void sched_enter_idle(void) __attribute__((noreturn));
