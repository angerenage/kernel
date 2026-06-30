#pragma once

#include <base/cap.h>
#include <base/process.h>
#include <base/syscall.h>
#include <stddef.h>
#include <stdint.h>

/* Create a process and return caller-owned capabilities for it and its address space. */
syscall_status_t process_create(const char* name, size_t name_length, struct process_create_response* out_response);

/* Read the calling process's identity, address-space cap and main thread cap. */
syscall_status_t process_self_info(struct self_info* out_info);

/* Read info about a process through its capability. */
syscall_status_t process_get_info(cap_id_t cap, struct process_info_response* out_info);

/* Copy an argument onto the new main thread's stack and start it. */
syscall_status_t process_run(cap_id_t cap, uintptr_t entry, const void* arg_data, size_t arg_size,
                             cap_id_t* out_thread_cap);

/* Copy an argument onto a new joinable thread's stack and start it. */
syscall_status_t process_spawn_thread(cap_id_t cap, uintptr_t entry, const void* arg_data, size_t arg_size,
                                      const char* name, size_t name_length, cap_id_t* out_thread_cap);

/* Wait for a process to terminate through its capability. */
syscall_status_t process_wait(cap_id_t cap, uintptr_t* out_exit_code);

/* Detach a process through its capability. */
syscall_status_t process_detach(cap_id_t cap);

/* Kill a process through its capability. */
syscall_status_t process_kill(cap_id_t cap, uintptr_t exit_code);

/* Terminate the calling process. Loops on either the process or thread exit syscall to handle races. */
__attribute__((noreturn))
void process_exit(uintptr_t code);
