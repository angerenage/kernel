#pragma once

#include <stdbool.h>
#include <stddef.h>

struct cpu;
struct thread;

/*
 * Minimal per-CPU scheduler front-end.
 *
 * Each CPU owns a run queue plus a permanently available idle thread. The
 * current implementation does not perform preemption or context switching yet;
 * it mainly provides the queueing model that later scheduling code will build on.
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

/* Queue a runnable thread onto a CPU's run queue. */
bool sched_enqueue(struct cpu* cpu, struct thread* thread);

/* Pop the next runnable thread for a CPU, or that CPU's idle thread if the queue is empty. */
struct thread* sched_dequeue_next(struct cpu* cpu);

/* Return the current run-queue depth for a CPU. */
size_t sched_run_queue_depth(struct cpu* cpu);

/* Enter the CPU's idle loop and never return. */
void sched_enter_idle(void) __attribute__((noreturn));
