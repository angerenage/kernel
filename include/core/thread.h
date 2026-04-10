#pragma once

#include <core/spinlock.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct cpu;

/* High-level scheduler state for a thread descriptor. */
enum thread_state {
	THREAD_STATE_NEW = 0,
	THREAD_STATE_READY,
	THREAD_STATE_RUNNING,
	THREAD_STATE_BLOCKED,
	THREAD_STATE_IDLE,
};

enum thread_flags {
	THREAD_FLAG_NONE   = 0u,
	THREAD_FLAG_IDLE   = 1u << 0,
	THREAD_FLAG_QUEUED = 1u << 1,
};

/* Architecture-neutral execution context fields tracked today. */
struct thread_context {
	uintptr_t instruction_pointer;
	uintptr_t stack_pointer;
};

/* Kernel thread descriptor used by the current scheduler prototype. */
struct thread {
	const char*           name;
	struct cpu*           cpu;
	enum thread_state     state;
	uint32_t              flags;
	uintptr_t             kernel_stack_base;
	uintptr_t             kernel_stack_top;
	struct thread_context context;
	struct thread*        run_queue_next;
};

/* FIFO run queue protected by a spinlock. */
struct run_queue {
	struct spinlock lock;
	struct thread*  head;
	struct thread*  tail;
	size_t          depth;
};

/* Initialize a regular thread descriptor with its kernel stack bounds. */
void thread_init(struct thread* thread, const char* name, uintptr_t kernel_stack_base, uintptr_t kernel_stack_top);

/* Initialize a CPU's dedicated idle thread descriptor. */
void thread_init_idle(struct thread* thread, struct cpu* cpu, const char* name);

/* Return true for the special per-CPU idle thread. */
bool thread_is_idle(const struct thread* thread);

/* Return true while the thread is linked into a run queue. */
bool thread_is_queued(const struct thread* thread);

/* Mark a thread running on cpu and clear any queue linkage. */
void thread_mark_running(struct thread* thread, struct cpu* cpu);

/* Mark a thread blocked and remove any queue linkage. */
void thread_mark_blocked(struct thread* thread);

/* Initialize an empty FIFO run queue. */
void run_queue_init(struct run_queue* queue);

/* Enqueue a non-idle thread at the tail of the queue. */
bool run_queue_enqueue(struct run_queue* queue, struct thread* thread);

/* Remove and return the head thread, or NULL when the queue is empty. */
struct thread* run_queue_dequeue(struct run_queue* queue);

/* Return the current number of queued threads. */
size_t run_queue_depth(struct run_queue* queue);
