#pragma once

#include <core/thread.h>
#include <core/vmm.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	KTHREAD_DEFAULT_STACK_PAGES = 4u,
};

enum kthread_spawn_result {
	KTHREAD_SPAWN_OK = 0,
	KTHREAD_SPAWN_INVALID_ARGUMENTS,
	KTHREAD_SPAWN_NO_MEMORY,
	KTHREAD_SPAWN_STACK_ALLOC_FAILED,
	KTHREAD_SPAWN_CONTEXT_UNSUPPORTED,
	KTHREAD_SPAWN_START_FAILED,
	KTHREAD_SPAWN_REAPER_UNAVAILABLE,
};

struct kthread {
	struct thread   thread;
	vmm_id_t        stack_id;
	size_t          stack_pages;
	struct kthread* reaper_next;
};

/* Return the thread descriptor currently associated with the running CPU. */
struct thread* kthread_current(void);

/* Spawn a joinable kernel thread on the scheduler's default CPU. */
enum kthread_spawn_result kthread_spawn(struct kthread** out_thread, const char* name, thread_entry_t entry, void* arg);

/* Spawn a joinable kernel thread with a CPU preference. */
enum kthread_spawn_result kthread_spawn_on_cpu(struct kthread** out_thread, const char* name, thread_entry_t entry,
                                               void* arg, struct cpu* preferred_cpu);

/* Spawn a detached kernel thread on the scheduler's default CPU. */
enum kthread_spawn_result kthread_spawn_detached(const char* name, thread_entry_t entry, void* arg);

/* Spawn a detached kernel thread with a CPU preference. */
enum kthread_spawn_result kthread_spawn_detached_on_cpu(const char* name, thread_entry_t entry, void* arg,
                                                        struct cpu* preferred_cpu);

/* Start the detached-thread reaper. Usually called lazily by detached spawn/detach. */
bool kthread_reaper_start(struct cpu* preferred_cpu);

/* Block until target exits (or is already exited), then publish its exit code. */
bool kthread_join(struct kthread* target, thread_exit_code_t* out_exit_code);

/* Reclaim a terminated joinable kthread handle. */
bool kthread_destroy(struct kthread* target);

/* Mark target as detached so no thread may join it; the reaper will reclaim it after exit. */
bool kthread_detach(struct kthread* target);

/* Request deferred cancellation on target thread. */
bool kthread_cancel(struct kthread* target);

/* Exit the current thread when a deferred cancellation request is pending. */
void kthread_testcancel(void);

/* Yield the current thread's CPU slot to the next runnable thread. */
void kthread_yield(void);

/* Sleep for at least ms milliseconds using the kernel periodic timer source. */
bool kthread_sleep_ms(uint64_t ms);

/* Publish the current thread exit code and hand control back to the scheduler. */
__attribute__((noreturn))
void kthread_exit(thread_exit_code_t exit_code);
