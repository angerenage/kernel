#pragma once

#include <base/cap.h>
#include <base/channel.h>
#include <base/module.h>
#include <base/self.h>
#include <base/syscall.h>
#include <base/vmm.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

syscall_result_t syscall(uintptr_t number, uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
                         uintptr_t arg4, uintptr_t arg5);

/* Exercise the syscall path without changing kernel state. */
static inline syscall_result_t nop(void) {
	return syscall(SYSCALL_NOP, 0u, 0u, 0u, 0u, 0u, 0u);
}

/* Yield the current thread's remaining scheduler time. */
static inline syscall_result_t yield(void) {
	return syscall(SYSCALL_YIELD, 0u, 0u, 0u, 0u, 0u, 0u);
}

/* Block the current thread for at least the requested number of milliseconds. */
static inline syscall_result_t sleep_ms(uintptr_t milliseconds) {
	return syscall(SYSCALL_SLEEP_MS, milliseconds, 0u, 0u, 0u, 0u, 0u);
}

/* Return the scheduler tick count. */
static inline syscall_result_t tick_count(void) {
	return syscall(SYSCALL_TICK_COUNT, 0u, 0u, 0u, 0u, 0u, 0u);
}

/* Return a struct self_info describing the calling process and a capability to it. */
static inline syscall_result_t self(struct self_info* info) {
	return syscall(SYSCALL_SELF, (uintptr_t)info, 0u, 0u, 0u, 0u, 0u);
}

/* Terminate the current process with the supplied exit code. */
static inline syscall_result_t exit_process(uintptr_t code) {
	return syscall(SYSCALL_EXIT_PROCESS, code, 0u, 0u, 0u, 0u, 0u);
}

/* Create a process record and return a capability that grants full process-management rights over the new process. */
static inline syscall_result_t create_process(const char* name, size_t name_length) {
	return syscall(SYSCALL_CREATE_PROCESS, (uintptr_t)name, (uintptr_t)name_length, 0u, 0u, 0u, 0u);
}

/* Terminate the current thread with the supplied exit code. */
static inline syscall_result_t exit_thread(uintptr_t code) {
	return syscall(SYSCALL_EXIT_THREAD, code, 0u, 0u, 0u, 0u, 0u);
}

/* Spawn a thread in the current process at a user entry point. */
static inline syscall_result_t spawn_thread(uintptr_t entry, uintptr_t arg, size_t stack_pages, bool detached,
                                            const char* name, size_t name_length) {
	return syscall(SYSCALL_SPAWN_THREAD,
	               entry,
	               arg,
	               (uintptr_t)stack_pages,
	               detached ? 1u : 0u,
	               (uintptr_t)name,
	               (uintptr_t)name_length);
}

/* Wait for a thread in the current process to exit and return its exit code. */
static inline syscall_result_t join_thread(uintptr_t tid) {
	return syscall(SYSCALL_JOIN_THREAD, tid, 0u, 0u, 0u, 0u, 0u);
}

/* Detach a thread in the current process so it can be reclaimed without being joined. */
static inline syscall_result_t detach_thread(uintptr_t tid) {
	return syscall(SYSCALL_DETACH_THREAD, tid, 0u, 0u, 0u, 0u, 0u);
}

/* Request deferred cancellation of a thread in the current process. */
static inline syscall_result_t cancel_thread(uintptr_t tid) {
	return syscall(SYSCALL_CANCEL_THREAD, tid, 0u, 0u, 0u, 0u, 0u);
}

/* Enable or disable deferred cancellation for the current thread. */
static inline syscall_result_t set_thread_cancel_enabled(bool enabled) {
	return syscall(SYSCALL_SET_THREAD_CANCEL_ENABLED, enabled ? 1u : 0u, 0u, 0u, 0u, 0u, 0u);
}

/* Exit the current thread if it has a pending cancellation request. */
static inline syscall_result_t test_thread_cancel(void) {
	return syscall(SYSCALL_TEST_THREAD_CANCEL, 0u, 0u, 0u, 0u, 0u, 0u);
}

/* Send a message to a target process. */
static inline syscall_result_t send_message(uintptr_t pid, const void* buffer, size_t length) {
	return syscall(SYSCALL_SEND_MESSAGE, pid, (uintptr_t)buffer, (uintptr_t)length, 0u, 0u, 0u);
}

/* Receive the next message into buffer, writing the message size and sender PID. */
static inline syscall_result_t recv_message(void* buffer, size_t buffer_size, size_t* out_length,
                                            uintptr_t* out_sender_pid) {
	return syscall(SYSCALL_RECV_MESSAGE,
	               (uintptr_t)buffer,
	               (uintptr_t)out_length,
	               (uintptr_t)buffer_size,
	               (uintptr_t)out_sender_pid,
	               0u,
	               0u);
}

/* Create a new channel and return its ID. */
static inline syscall_result_t channel_create(channel_id_t* out_id) {
	return syscall(SYSCALL_CHANNEL_CREATE, (uintptr_t)out_id, 0u, 0u, 0u, 0u, 0u);
}

/* Destroy a channel by ID. */
static inline syscall_result_t channel_destroy(channel_id_t channel_id) {
	return syscall(SYSCALL_CHANNEL_DESTROY, (uintptr_t)channel_id, 0u, 0u, 0u, 0u, 0u);
}

/* Resolve a boot module by name and get a capability to map it. */
static inline syscall_result_t module_resolve(const char* name, size_t name_length, process_id_t target) {
	return syscall(SYSCALL_MODULE_RESOLVE, (uintptr_t)name, (uintptr_t)name_length, (uintptr_t)target, 0u, 0u, 0u);
}

/* Create a capability for an object owned by the caller's endpoint. */
static inline syscall_result_t cap_create(channel_id_t endpoint_id, process_id_t target, uint64_t object_id,
                                          cap_rights_t rights, cap_id_t* out) {
	return syscall(SYSCALL_CAP_CREATE,
	               (uintptr_t)endpoint_id,
	               (uintptr_t)target,
	               (uintptr_t)object_id,
	               (uintptr_t)rights,
	               (uintptr_t)out,
	               0u);
}

/* Delegate a capability to another entity with a subset of rights. */
static inline syscall_result_t cap_delegate(cap_id_t source, process_id_t target, cap_rights_t rights, cap_id_t* out) {
	return syscall(
		SYSCALL_CAP_DELEGATE, (uintptr_t)source, (uintptr_t)target, (uintptr_t)rights, (uintptr_t)out, 0u, 0u);
}

/* Derive a capability for a different object under the same endpoint. */
static inline syscall_result_t cap_derive(cap_id_t base, process_id_t target, uint64_t object_id, cap_rights_t rights,
                                          cap_id_t* out) {
	return syscall(SYSCALL_CAP_DERIVE,
	               (uintptr_t)base,
	               (uintptr_t)target,
	               (uintptr_t)object_id,
	               (uintptr_t)rights,
	               (uintptr_t)out,
	               0u);
}

/* Deliver a request payload to the capability's endpoint. */
static inline syscall_result_t cap_call(cap_id_t cap, const void* request, size_t request_size) {
	return syscall(SYSCALL_CAP_CALL, (uintptr_t)cap, (uintptr_t)request, (uintptr_t)request_size, 0u, 0u, 0u);
}

/* Revoke rights from a capability, or revoke the capability entirely (rights == 0). */
static inline syscall_result_t cap_revoke(cap_id_t cap, cap_rights_t rights) {
	return syscall(SYSCALL_CAP_REVOKE, (uintptr_t)cap, (uintptr_t)rights, 0u, 0u, 0u, 0u);
}

/* Dequeue a capability call request from an endpoint. */
static inline syscall_result_t cap_recv(channel_id_t endpoint_id, struct cap_request* out, void* request_buffer,
                                        size_t request_buffer_size) {
	return syscall(SYSCALL_CAP_RECV,
	               (uintptr_t)endpoint_id,
	               (uintptr_t)out,
	               (uintptr_t)request_buffer,
	               (uintptr_t)request_buffer_size,
	               0u,
	               0u);
}
