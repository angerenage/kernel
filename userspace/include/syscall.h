#pragma once

#include <base/channel.h>
#include <base/module.h>
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

/* Write a user buffer to the kernel's temporary diagnostic output. */
static inline syscall_result_t print(const char* data, size_t length) {
	return syscall(SYSCALL_PRINT, (uintptr_t)data, (uintptr_t)length, 0u, 0u, 0u, 0u);
}

/* Return the current process identifier. */
static inline syscall_result_t getpid(void) {
	return syscall(SYSCALL_GETPID, 0u, 0u, 0u, 0u, 0u, 0u);
}

/* Return the number of threads currently owned by the current process. */
static inline syscall_result_t get_process_thread_count(void) {
	return syscall(SYSCALL_GET_PROCESS_THREAD_COUNT, 0u, 0u, 0u, 0u, 0u, 0u);
}

/* Return the current user thread identifier. */
static inline syscall_result_t gettid(void) {
	return syscall(SYSCALL_GETTID, 0u, 0u, 0u, 0u, 0u, 0u);
}

/* Terminate the current process with the supplied exit code. */
static inline syscall_result_t exit_process(uintptr_t code) {
	return syscall(SYSCALL_EXIT_PROCESS, code, 0u, 0u, 0u, 0u, 0u);
}

/* Create a process record using the supplied NUL-terminated name buffer. */
static inline syscall_result_t create_process(const char* name, size_t name_length) {
	return syscall(SYSCALL_CREATE_PROCESS, (uintptr_t)name, (uintptr_t)name_length, 0u, 0u, 0u, 0u);
}

/* Start a process's main thread at a user entry point. */
static inline syscall_result_t run_process(uintptr_t pid, uintptr_t entry, uintptr_t arg, size_t stack_pages) {
	return syscall(SYSCALL_RUN_PROCESS, pid, entry, arg, (uintptr_t)stack_pages, 0u, 0u);
}

/* Wait for a process to exit and return its exit code. */
static inline syscall_result_t wait_process(uintptr_t pid) {
	return syscall(SYSCALL_WAIT_PROCESS, pid, 0u, 0u, 0u, 0u, 0u);
}

/* Detach a process so it can be reclaimed without being waited on. */
static inline syscall_result_t detach_process(uintptr_t pid) {
	return syscall(SYSCALL_DETACH_PROCESS, pid, 0u, 0u, 0u, 0u, 0u);
}

/* Request termination of a process with the supplied exit code. */
static inline syscall_result_t kill_process(uintptr_t pid, uintptr_t code) {
	return syscall(SYSCALL_KILL_PROCESS, pid, code, 0u, 0u, 0u, 0u);
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

/* Reserve virtual pages in a target process and optionally copy out the chosen base address. */
static inline syscall_result_t vm_reserve(uintptr_t pid, size_t page_count, vmm_prot_t prot, enum vmm_kind kind,
                                          uint64_t map_flags, uintptr_t* out_base) {
	return syscall(SYSCALL_VM_RESERVE,
	               pid,
	               (uintptr_t)page_count,
	               (uintptr_t)prot,
	               (uintptr_t)kind,
	               (uintptr_t)map_flags,
	               (uintptr_t)out_base);
}

/* Reserve virtual pages at an explicit base in a target process. */
static inline syscall_result_t vm_reserve_at(uintptr_t pid, void* base, size_t page_count, vmm_prot_t prot,
                                             enum vmm_kind kind, uint64_t map_flags) {
	return syscall(SYSCALL_VM_RESERVE_AT,
	               pid,
	               (uintptr_t)base,
	               (uintptr_t)page_count,
	               (uintptr_t)prot,
	               (uintptr_t)kind,
	               (uintptr_t)map_flags);
}

/* Free a virtual memory allocation in a target process. */
static inline syscall_result_t vm_free(uintptr_t pid, vmm_id_t id) {
	return syscall(SYSCALL_VM_FREE, pid, (uintptr_t)id, 0u, 0u, 0u, 0u);
}

/* Materialize all pages for a virtual memory allocation in a target process. */
static inline syscall_result_t vm_map(uintptr_t pid, vmm_id_t id) {
	return syscall(SYSCALL_VM_MAP, pid, (uintptr_t)id, 0u, 0u, 0u, 0u);
}

/* Remove mappings for a virtual memory allocation in a target process. */
static inline syscall_result_t vm_unmap(uintptr_t pid, vmm_id_t id, bool release_phys) {
	return syscall(SYSCALL_VM_UNMAP, pid, (uintptr_t)id, release_phys ? 1u : 0u, 0u, 0u, 0u);
}

/* Change protection flags for a virtual memory allocation in a target process. */
static inline syscall_result_t vm_protect(uintptr_t pid, vmm_id_t id, vmm_prot_t prot) {
	return syscall(SYSCALL_VM_PROTECT, pid, (uintptr_t)id, (uintptr_t)prot, 0u, 0u, 0u);
}

/* Copy information about a virtual memory allocation into a user buffer. */
static inline syscall_result_t vm_query(uintptr_t pid, vmm_id_t id, struct vmm_info* out_info) {
	return syscall(SYSCALL_VM_QUERY, pid, (uintptr_t)id, (uintptr_t)out_info, 0u, 0u, 0u);
}

/* Copy bytes from a target process into the current process. */
static inline syscall_result_t vm_copy_from(uintptr_t src_pid, const void* src, void* dst, size_t size) {
	return syscall(SYSCALL_VM_COPY_FROM, src_pid, (uintptr_t)src, (uintptr_t)dst, (uintptr_t)size, 0u, 0u);
}

/* Copy bytes from the current process into a target process. */
static inline syscall_result_t vm_copy_to(uintptr_t dst_pid, void* dst, const void* src, size_t size) {
	return syscall(SYSCALL_VM_COPY_TO, dst_pid, (uintptr_t)dst, (uintptr_t)src, (uintptr_t)size, 0u, 0u);
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

/* Send a message to a channel by ID. */
static inline syscall_result_t channel_send(channel_id_t channel_id, const void* buffer, size_t length) {
	return syscall(SYSCALL_CHANNEL_SEND, (uintptr_t)channel_id, (uintptr_t)buffer, (uintptr_t)length, 0u, 0u, 0u);
}

/* Receive a message from a channel by ID. */
static inline syscall_result_t channel_recv(channel_id_t channel_id, void* buffer, size_t buffer_size,
                                            size_t* out_length, uintptr_t* out_sender_pid) {
	return syscall(SYSCALL_CHANNEL_RECV,
	               (uintptr_t)channel_id,
	               (uintptr_t)buffer,
	               (uintptr_t)buffer_size,
	               (uintptr_t)out_length,
	               (uintptr_t)out_sender_pid,
	               0u);
}

/* Destroy a channel by ID. */
static inline syscall_result_t channel_destroy(channel_id_t channel_id) {
	return syscall(SYSCALL_CHANNEL_DESTROY, (uintptr_t)channel_id, 0u, 0u, 0u, 0u, 0u);
}

/* Resolve a boot module by name and get its info. */
static inline syscall_result_t module_resolve(const char* name, size_t name_length, struct module_info* out_info) {
	return syscall(SYSCALL_MODULE_RESOLVE, (uintptr_t)name, (uintptr_t)name_length, (uintptr_t)out_info, 0u, 0u, 0u);
}

/* Map a boot module into the current process and return the virtual address. */
static inline syscall_result_t module_map(size_t index) {
	return syscall(SYSCALL_MODULE_MAP, index, 0u, 0u, 0u, 0u, 0u);
}
