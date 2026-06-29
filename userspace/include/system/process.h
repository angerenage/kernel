#pragma once

#include <base/cap.h>
#include <base/self.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Create a process and return caller-owned capabilities for it and its address space. */
bool process_create(const char* name, size_t name_length, struct process_create_response* out_response);

/* Read the calling process's identity, address-space cap and main thread cap. */
bool process_self_info(struct self_info* out_info);

/* Read info about a process through its capability. */
bool process_get_info(cap_id_t cap, struct process_info_response* out_info);

/* Start the main thread of a process through its capability. */
bool process_run(cap_id_t cap, uintptr_t entry, uintptr_t arg, size_t stack_pages, cap_id_t* out_thread_cap);

/* Spawn a joinable thread inside a process through its capability. */
bool process_spawn_thread(cap_id_t cap, uintptr_t entry, uintptr_t arg, size_t stack_pages, const char* name,
                          size_t name_length, cap_id_t* out_thread_cap);

/* Wait for a process to terminate through its capability. */
bool process_wait(cap_id_t cap, uintptr_t* out_exit_code);

/* Detach a process through its capability. */
bool process_detach(cap_id_t cap);

/* Kill a process through its capability. */
bool process_kill(cap_id_t cap, uintptr_t exit_code);

/* Terminate the calling process. Loops on either the process or thread exit syscall to handle races. */
__attribute__((noreturn))
void process_exit(uintptr_t code);
