#pragma once

#include <base/process.h>
#include <core/capability.h>
#include <core/channel.h>
#include <core/message.h>
#include <core/thread.h>
#include <core/vaddr_alloc.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct cpu;
struct uthread;

enum process_state {
	PROCESS_STATE_NEW = 0,
	PROCESS_STATE_RUNNING,
	PROCESS_STATE_EXITING,
	PROCESS_STATE_ZOMBIE,
};

enum process_result {
	PROCESS_OK = 0,
	PROCESS_INVALID_ARGUMENTS,
	PROCESS_NO_MEMORY,
	PROCESS_ADDRESS_SPACE_FAILED,
	PROCESS_PID_EXHAUSTED,
	PROCESS_THREAD_STACK_ALLOC_FAILED,
	PROCESS_THREAD_CONTEXT_UNSUPPORTED,
	PROCESS_THREAD_SCHEDULER_REJECTED,
	PROCESS_THREAD_REAPER_UNAVAILABLE,
	PROCESS_THREAD_ID_EXHAUSTED,
};

enum process_join_result {
	PROCESS_JOIN_OK = 0,
	PROCESS_JOIN_INVALID_ARGUMENTS,
	PROCESS_JOIN_SELF,
	PROCESS_JOIN_DETACHED,
	PROCESS_JOIN_ALREADY_JOINED,
	PROCESS_JOIN_WAIT_FAILED,
};

enum process_detach_result {
	PROCESS_DETACH_OK = 0,
	PROCESS_DETACH_INVALID_ARGUMENTS,
	PROCESS_DETACH_ALREADY_DETACHED,
	PROCESS_DETACH_ALREADY_JOINED,
};

enum process_thread_join_result {
	PROCESS_THREAD_JOIN_OK = 0,
	PROCESS_THREAD_JOIN_INVALID_ARGUMENTS,
	PROCESS_THREAD_JOIN_FOREIGN_THREAD,
	PROCESS_THREAD_JOIN_SELF,
	PROCESS_THREAD_JOIN_DETACHED,
	PROCESS_THREAD_JOIN_WAIT_FAILED,
	PROCESS_THREAD_JOIN_RECLAIM_FAILED,
};

enum process_thread_detach_result {
	PROCESS_THREAD_DETACH_OK = 0,
	PROCESS_THREAD_DETACH_INVALID_ARGUMENTS,
	PROCESS_THREAD_DETACH_FOREIGN_THREAD,
	PROCESS_THREAD_DETACH_ALREADY_DETACHED,
	PROCESS_THREAD_DETACH_ALREADY_TERMINATED,
	PROCESS_THREAD_DETACH_FAILED,
};

enum process_thread_cancel_result {
	PROCESS_THREAD_CANCEL_OK = 0,
	PROCESS_THREAD_CANCEL_INVALID_ARGUMENTS,
	PROCESS_THREAD_CANCEL_FOREIGN_THREAD,
	PROCESS_THREAD_CANCEL_ALREADY_TERMINATED,
	PROCESS_THREAD_CANCEL_FAILED,
};

enum process_thread_spawn_result {
	PROCESS_THREAD_SPAWN_OK = 0,
	PROCESS_THREAD_SPAWN_INVALID_ARGUMENTS,
	PROCESS_THREAD_SPAWN_NO_MEMORY,
	PROCESS_THREAD_SPAWN_STACK_ALLOC_FAILED,
	PROCESS_THREAD_SPAWN_CONTEXT_UNSUPPORTED,
	PROCESS_THREAD_SPAWN_SCHEDULER_REJECTED,
	PROCESS_THREAD_SPAWN_REAPER_UNAVAILABLE,
	PROCESS_THREAD_SPAWN_ID_EXHAUSTED,
};

struct process_spawn_params {
	const char* name;
	uintptr_t   user_entry;
	uintptr_t   user_arg;
	size_t      user_stack_pages;
	struct cpu* preferred_cpu;
};

struct process_thread_params {
	const char* name;
	uintptr_t   user_entry;
	uintptr_t   user_arg;
	size_t      user_stack_pages;
	struct cpu* preferred_cpu;
	bool        detached;
};

/* Aggregate state of one process: its address space, threads, and lifecycle bookkeeping. */
struct process {
	process_id_t       pid;
	const char*        name;
	enum process_state state;
	/* Exit code produced when the process terminates. */
	uintptr_t exit_code;
	size_t    thread_count;
	/* Parent process that created this one, or NULL for the initial process. */
	struct process* parent;
	struct uthread* main_thread;
	/* Singly-linked list of all userspace threads, head and tail. */
	struct uthread* thread_head;
	struct uthread* thread_tail;
	/* Wait queue for threads blocking on process join. */
	struct thread_wait_queue join_wait_queue;
	/* Channel state and message ring buffer for inter-process communication. */
	struct ring_buffer           message_queue;
	struct process_channel_state channel_state;
	/* User-mode address space for this process. */
	struct address_space address_space;
	struct spinlock      lock;
	/* When true, the process cannot be joined anymore. */
	bool detached;
	/* When true, some thread has successfully joined this process. */
	bool joined;
	/* Lazily-created cap_object id for this process; CAP_OBJECT_ID_INVALID until one is created. */
	cap_object_id_t cap_object_id;
	/* Lazily-created cap_object id for this process' address space. */
	cap_object_id_t address_space_cap_object_id;
};

/* Allocate, initialize, and queue a userspace thread inside process. Detached threads do not publish a handle. */
enum process_thread_spawn_result process_spawn_thread(struct process* process, struct uthread** out_thread,
                                                      const struct process_thread_params* params);

/* Queue the initial userspace thread for a newly-created process. */
enum process_thread_spawn_result process_start_main_thread(struct process* process, struct uthread** out_thread,
                                                           const struct process_thread_params* params);

/* Block until thread exits, publish its exit code, and reclaim its process-owned resources. */
enum process_thread_join_result process_join_thread(struct process* process, struct uthread* thread,
                                                    uintptr_t* out_exit_code);

/* Mark a process thread detached so it is reclaimed by the user-thread reaper after exit. */
enum process_thread_detach_result process_detach_thread(struct process* process, struct uthread* thread);

/* Request deferred cancellation of a process thread. */
enum process_thread_cancel_result process_cancel_thread(struct process* process, struct uthread* thread);

/* Allocate and initialize a process, but do not start it. */
enum process_result process_create(struct process** out_process, const char* name);

/* Block until process exits, then publish its process exit code. */
enum process_join_result process_join(struct process* process, uintptr_t* out_exit_code);

/* Mark process as detached so it can no longer be joined. */
enum process_detach_result process_detach(struct process* process);

/* Request termination of all threads in process and publish the process exit code. */
bool process_terminate(struct process* process, uintptr_t exit_code);

/* Destroy all reclaimable process threads, release its user address space, and free the process. */
bool process_destroy(struct process* process);

/* Return a process PID, or PROCESS_PID_INVALID for NULL. */
process_id_t process_pid(const struct process* process);

/* Return the process registered for pid, or NULL when pid is invalid or absent. */
struct process* process_lookup(process_id_t pid);

/* Return the number of registered processes. */
size_t process_count(void);

/* Return the mutable user address space owned by process. */
struct address_space* process_address_space(struct process* process);

/* Return the main userspace thread for process, or NULL. */
struct uthread* process_main_thread(struct process* process);

/* Return the current process state. */
enum process_state process_get_state(struct process* process);

/* Return the number of threads currently attached to process. */
size_t process_thread_count(struct process* process);

/* Return the process owned by the current userspace thread, or NULL. */
struct process* process_current(void);

/* Update process lifecycle after one of its scheduler threads exits. */
void process_notify_thread_exit(struct process* process, struct thread* thread, uintptr_t exit_code);

/* Return the lazily-allocated cap_object id cached on this process, or CAP_OBJECT_ID_INVALID when none has been
 * created. */
cap_object_id_t process_cap_object_id(const struct process* process);

/* Publish a cap_object id on the process. Pass CAP_OBJECT_ID_INVALID to clear the slot. */
void process_set_cap_object_id(struct process* process, cap_object_id_t id);

/* Destroy the lazily-created cap_object for this process if one is cached and reset the slot. Returns true when
 * destroyed. */
bool process_destroy_cap_object(struct process* process);

/* Return the lazily-allocated cap_object id for the process address space. */
cap_object_id_t process_address_space_cap_object_id(const struct process* process);

/* Publish an address-space cap_object id on the process. */
void process_set_address_space_cap_object_id(struct process* process, cap_object_id_t id);

/* Destroy the lazily-created address-space cap_object and reset its slot. */
bool process_destroy_address_space_cap_object(struct process* process);
