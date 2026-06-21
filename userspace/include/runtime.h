#pragma once

#include <base/cap.h>
#include <base/self.h>
#include <stdbool.h>
#include <stdint.h>

int main(int argc, char** argv);

__attribute__((noreturn))
void exit(uintptr_t code);

/* Get process information through a capability. Returns true on success. */
bool process_get_info(cap_id_t cap, struct process_info_response* out_info);

/* Run a process through a capability. Returns true on success. */
bool process_run(cap_id_t cap, uintptr_t entry, uintptr_t arg, size_t stack_pages);

/* Wait for process termination through a capability. Returns true on success. */
bool process_wait(cap_id_t cap, uintptr_t* out_exit_code);

/* Detach a process through a capability. Returns true on success. */
bool process_detach(cap_id_t cap);

/* Kill a process through a capability. Returns true on success. */
bool process_kill(cap_id_t cap, uintptr_t exit_code);
