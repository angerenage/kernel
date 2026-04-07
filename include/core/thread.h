#pragma once

#include <core/spinlock.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct cpu;

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

struct thread_context {
	uintptr_t instruction_pointer;
	uintptr_t stack_pointer;
};

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

struct run_queue {
	struct spinlock lock;
	struct thread*  head;
	struct thread*  tail;
	size_t          depth;
};

void thread_init(struct thread* thread, const char* name, uintptr_t kernel_stack_base, uintptr_t kernel_stack_top);
void thread_init_idle(struct thread* thread, struct cpu* cpu, const char* name);
bool thread_is_idle(const struct thread* thread);
bool thread_is_queued(const struct thread* thread);
void thread_mark_running(struct thread* thread, struct cpu* cpu);
void thread_mark_blocked(struct thread* thread);

void           run_queue_init(struct run_queue* queue);
bool           run_queue_enqueue(struct run_queue* queue, struct thread* thread);
struct thread* run_queue_dequeue(struct run_queue* queue);
size_t         run_queue_depth(struct run_queue* queue);
