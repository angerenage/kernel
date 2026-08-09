#pragma once

#include <base/thread.h>
#include <core/spinlock.h>
#include <hal/cpu.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct cpu;
struct address_space;
struct mutex;
struct thread;
struct thread_wait_queue;

enum {
	THREAD_DEFAULT_TIMESLICE_TICKS = 4u,
};

enum {
	THREAD_PRIORITY_MIN     = -20,
	THREAD_PRIORITY_DEFAULT = 0,
	THREAD_PRIORITY_MAX     = 20,
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
	THREAD_FLAG_NONE               = 0u,
	THREAD_FLAG_IDLE               = 1u << 0,
	THREAD_FLAG_QUEUED             = 1u << 1,
	THREAD_FLAG_DETACHED           = 1u << 2,
	THREAD_FLAG_CANCEL_PENDING     = 1u << 3,
	THREAD_FLAG_CANCEL_DISABLED    = 1u << 4,
	THREAD_FLAG_PARK_PERMIT        = 1u << 5,
	THREAD_FLAG_INTERRUPT_PENDING  = 1u << 6,
	THREAD_FLAG_WAIT_INTERRUPTIBLE = 1u << 7,
};

/* Why a thread is blocked when state == THREAD_STATE_BLOCKED. */
enum thread_block_reason {
	THREAD_BLOCK_NONE = 0,
	THREAD_BLOCK_WAIT_QUEUE,
	THREAD_BLOCK_SEMAPHORE,
	THREAD_BLOCK_SIGNAL,
	THREAD_BLOCK_MUTEX,
	THREAD_BLOCK_RWLOCK,
	THREAD_BLOCK_JOIN,
	THREAD_BLOCK_SLEEP,
	THREAD_BLOCK_PARKED,
};

/* Higher-level object that owns a scheduler thread descriptor. */
enum thread_owner_kind {
	THREAD_OWNER_NONE = 0,
	THREAD_OWNER_KTHREAD,
	THREAD_OWNER_UTHREAD,
};

/* Resolution state for waits that are armed against both a wait queue and a timeout. */
enum thread_wait_status {
	THREAD_WAIT_STATUS_NONE = 0,
	THREAD_WAIT_STATUS_PENDING,
	THREAD_WAIT_STATUS_SIGNALED,
	THREAD_WAIT_STATUS_TIMED_OUT,
	THREAD_WAIT_STATUS_CANCELED,
	THREAD_WAIT_STATUS_INTERRUPTED,
};

/* Parameters used to initialize a non-idle thread descriptor. */
struct thread_create_params {
	const char*           name;
	thread_entry_t        entry;
	void*                 arg;
	uintptr_t             kernel_stack_base;
	uintptr_t             kernel_stack_top;
	struct address_space* address_space;
	struct cpu*           preferred_cpu;
	int32_t               base_priority;
	bool                  detached;
};

/* Parameters used when the caller already built an initial CPU context. */
struct thread_context_params {
	const char*                  name;
	uintptr_t                    kernel_stack_base;
	uintptr_t                    kernel_stack_top;
	struct address_space*        address_space;
	struct cpu*                  preferred_cpu;
	int32_t                      base_priority;
	bool                         detached;
	const struct thread_context* context;
	thread_entry_t               entry;
	void*                        arg;
};

/* Owner callback invoked from a safe stack after the scheduler publishes ZOMBIE. */
typedef void (*thread_reap_callback_t)(struct thread* thread, void* ctx);

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
	struct address_space*     address_space;
	enum thread_owner_kind    owner_kind;
	void*                     owner;
	struct thread_context     context;
	thread_entry_t            entry;
	void*                     arg;
	thread_exit_code_t        exit_code;
	struct thread_wait_queue  join_wait_queue;
	struct thread_wait_queue  park_wait_queue;
	struct thread_wait_queue* blocked_queue;
	struct mutex*             owned_mutexes;
	struct thread*            run_queue_next;
	struct thread*            wait_queue_next;
	struct thread*            sleep_queue_next;
	uint64_t                  wake_deadline_tick;
	uint32_t                  wait_status;
	int32_t                   base_priority;
	int32_t                   effective_priority;
	uint32_t                  timeslice_ticks;
	uint32_t                  timeslice_remaining;
	thread_reap_callback_t    reap_callback;
	void*                     reap_context;
};

/* Priority-ordered run queue with FIFO ordering among equal-priority threads. */
struct run_queue {
	struct spinlock lock;
	struct thread*  head;
	struct thread*  tail;
	size_t          depth;
};

enum thread_init_result {
	THREAD_INIT_OK = 0,
	THREAD_INIT_INVALID_ARGUMENTS,
	THREAD_INIT_INVALID_STACK,
	THREAD_INIT_CONTEXT_UNSUPPORTED,
};

/* Initialize a regular thread descriptor and return a classified failure result on rejection. */
enum thread_init_result thread_init_ex(struct thread* thread, const struct thread_create_params* params);

/* Initialize a thread descriptor from a prebuilt CPU context. */
enum thread_init_result thread_init_context(struct thread* thread, const struct thread_context_params* params);

/* Clamp a scheduler priority into the supported thread priority range. */
int32_t thread_priority_clamp(int32_t priority);

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

/* Return true once the thread no longer executes on, or may return to, its kernel stack. */
bool thread_is_reap_safe(const struct thread* thread);

/* Return true while another thread may still legally join this thread. */
bool thread_is_joinable(const struct thread* thread);

/* Return true when deferred cancellation has been requested for the thread. */
bool thread_cancel_requested(const struct thread* thread);

/* Return true when deferred cancellation checks are currently unmasked. */
bool thread_cancel_enabled(const struct thread* thread);

/* Return true when the thread should exit at the next cancellation point. */
bool thread_should_cancel(const struct thread* thread);

/* Mark an asynchronous userspace interruption pending without waking the thread. */
void thread_request_interrupt(struct thread* thread);

/* Return whether an asynchronous userspace interruption is pending. */
bool thread_interrupt_pending(const struct thread* thread);

/* Clear a pending userspace interruption after its upcall was delivered or purged. */
void thread_clear_interrupt(struct thread* thread);

/* Convert a joinable thread into a detached thread before it reaches ZOMBIE. */
bool thread_detach(struct thread* thread);

/* Record a deferred cancellation request on the target thread. */
bool thread_request_cancel(struct thread* thread);

/* Mask or unmask deferred cancellation checks for a thread. */
void thread_set_cancel_enabled(struct thread* thread, bool enabled);

/* Register owner-specific cleanup invoked after the thread has left its kernel stack. */
void thread_set_reap_callback(struct thread* thread, thread_reap_callback_t callback, void* ctx);

/* Invoke owner-specific cleanup from a stack that is safe for reclamation. */
void thread_notify_reap(struct thread* thread);

/* Mark a thread ready on cpu and clear any queue / wait linkage. */
void thread_mark_ready(struct thread* thread, struct cpu* cpu);

/* Mark a thread running on cpu and clear any queue / wait linkage. */
void thread_mark_running(struct thread* thread, struct cpu* cpu);

/* Mark a thread blocked for reason and remove any queue linkage. */
void thread_mark_blocked(struct thread* thread, enum thread_block_reason reason);

/* Publish a thread's exit status before the scheduler switches away from its stack. */
void thread_mark_exiting(struct thread* thread, thread_exit_code_t exit_code);

/* Mark a thread reap-safe after it has left the CPU and no longer uses its kernel stack. */
void thread_mark_zombie(struct thread* thread);

/* Initialize an empty FIFO wait queue. */
void thread_wait_queue_init(struct thread_wait_queue* queue);

/* Return the current number of blocked waiters in a thread wait queue. */
size_t thread_wait_queue_depth(struct thread_wait_queue* queue);

/* Initialize an empty priority-aware run queue. */
void run_queue_init(struct run_queue* queue);

/* Enqueue a non-idle thread by effective priority, preserving FIFO ties. */
bool run_queue_enqueue(struct run_queue* queue, struct thread* thread);

/* Reposition an already queued thread after its effective priority changes. */
bool run_queue_requeue(struct run_queue* queue, struct thread* thread);

/* Remove and return the head thread, or NULL when the queue is empty. */
struct thread* run_queue_dequeue(struct run_queue* queue);

/* Return the current number of queued threads. */
size_t run_queue_depth(struct run_queue* queue);
