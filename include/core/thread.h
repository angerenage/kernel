#pragma once

#include <core/spinlock.h>
#include <hal/cpu.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct cpu;
struct thread_wait_queue;

enum {
	THREAD_DEFAULT_TIMESLICE_TICKS = 4u,
};

/* Thread entry function used for first-run bootstrap. */
typedef void (*thread_entry_t)(void* arg);

/* Machine-word-sized thread exit value published to joiners. */
typedef uintptr_t thread_exit_code_t;

/* High-level lifecycle state for a thread descriptor. */
enum thread_state {
	THREAD_STATE_NEW = 0,
	THREAD_STATE_READY,
	THREAD_STATE_RUNNING,
	THREAD_STATE_BLOCKED,
	THREAD_STATE_EXITING,
	THREAD_STATE_ZOMBIE,
	THREAD_STATE_IDLE,
};

/* Persistent thread attributes and transient scheduler bookkeeping flags. */
enum thread_flags {
	THREAD_FLAG_NONE            = 0u,
	THREAD_FLAG_IDLE            = 1u << 0,
	THREAD_FLAG_QUEUED          = 1u << 1,
	THREAD_FLAG_DETACHED        = 1u << 2,
	THREAD_FLAG_CANCEL_PENDING  = 1u << 3,
	THREAD_FLAG_CANCEL_DISABLED = 1u << 4,
};

/* Why a thread is blocked when state == THREAD_STATE_BLOCKED. */
enum thread_block_reason {
	THREAD_BLOCK_NONE = 0,
	THREAD_BLOCK_WAIT_QUEUE,
	THREAD_BLOCK_MUTEX,
	THREAD_BLOCK_JOIN,
	THREAD_BLOCK_SLEEP,
	THREAD_BLOCK_PARKED,
};

/* Resolution state for waits that are armed against both a wait queue and a timeout. */
enum thread_wait_status {
	THREAD_WAIT_STATUS_NONE = 0,
	THREAD_WAIT_STATUS_PENDING,
	THREAD_WAIT_STATUS_SIGNALED,
	THREAD_WAIT_STATUS_TIMED_OUT,
};

/* Parameters used to initialize a non-idle thread descriptor. */
struct thread_create_params {
	const char*    name;
	thread_entry_t entry;
	void*          arg;
	uintptr_t      kernel_stack_base;
	uintptr_t      kernel_stack_top;
	struct cpu*    preferred_cpu;
	bool           detached;
};

/* FIFO wait queue used by joins and other blocking synchronization points. */
struct thread_wait_queue {
	struct spinlock lock;
	struct thread*  head;
	struct thread*  tail;
	size_t          depth;
};

/* Core thread descriptor shared by scheduler, wait queues, and higher-level thread wrappers. */
struct thread {
	const char*               name;
	struct cpu*               cpu;
	struct cpu*               preferred_cpu;
	enum thread_state         state;
	enum thread_block_reason  block_reason;
	uint32_t                  flags;
	uintptr_t                 kernel_stack_base;
	uintptr_t                 kernel_stack_top;
	struct thread_context     context;
	thread_entry_t            entry;
	void*                     arg;
	thread_exit_code_t        exit_code;
	struct thread_wait_queue  join_wait_queue;
	struct thread_wait_queue* blocked_queue;
	struct thread*            run_queue_next;
	struct thread*            wait_queue_next;
	struct thread*            sleep_queue_next;
	uint64_t                  wake_deadline_tick;
	uint32_t                  wait_status;
	uint32_t                  timeslice_ticks;
	uint32_t                  timeslice_remaining;
};

/* FIFO run queue protected by a spinlock. */
struct run_queue {
	struct spinlock lock;
	struct thread*  head;
	struct thread*  tail;
	size_t          depth;
};

/* Initialize a regular thread descriptor. */
bool thread_init(struct thread* thread, const struct thread_create_params* params);

/* Initialize a CPU's dedicated idle thread descriptor. */
void thread_init_idle(struct thread* thread, struct cpu* cpu, const char* name);

/* Return true for the special per-CPU idle thread. */
bool thread_is_idle(const struct thread* thread);

/* Return true while the thread is linked into a run queue. */
bool thread_is_queued(const struct thread* thread);

/* Return true once the thread has published a stable final exit status. */
bool thread_is_terminated(const struct thread* thread);

/* Return true while another thread may still legally join this thread. */
bool thread_is_joinable(const struct thread* thread);

/* Return true when deferred cancellation has been requested for the thread. */
bool thread_cancel_requested(const struct thread* thread);

/* Convert a joinable thread into a detached thread before it reaches ZOMBIE. */
bool thread_detach(struct thread* thread);

/* Record a deferred cancellation request on the target thread. */
bool thread_request_cancel(struct thread* thread);

/* Mask or unmask deferred cancellation checks for a thread. */
void thread_set_cancel_enabled(struct thread* thread, bool enabled);

/* Mark a thread ready on cpu and clear any queue / wait linkage. */
void thread_mark_ready(struct thread* thread, struct cpu* cpu);

/* Mark a thread running on cpu and clear any queue / wait linkage. */
void thread_mark_running(struct thread* thread, struct cpu* cpu);

/* Mark a thread blocked for reason and remove any queue linkage. */
void thread_mark_blocked(struct thread* thread, enum thread_block_reason reason);

/* Publish a thread's exit status before the scheduler performs final wakeups. */
void thread_mark_exiting(struct thread* thread, thread_exit_code_t exit_code);

/* Mark a thread as a zombie after it has woken joiners and left the CPU. */
void thread_mark_zombie(struct thread* thread);

/* Initialize an empty FIFO wait queue. */
void thread_wait_queue_init(struct thread_wait_queue* queue);

/* Return the current number of blocked waiters in a thread wait queue. */
size_t thread_wait_queue_depth(struct thread_wait_queue* queue);

/* Initialize an empty FIFO run queue. */
void run_queue_init(struct run_queue* queue);

/* Enqueue a non-idle thread at the tail of the queue. */
bool run_queue_enqueue(struct run_queue* queue, struct thread* thread);

/* Remove and return the head thread, or NULL when the queue is empty. */
struct thread* run_queue_dequeue(struct run_queue* queue);

/* Return the current number of queued threads. */
size_t run_queue_depth(struct run_queue* queue);
