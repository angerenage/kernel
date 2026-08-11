#pragma once

struct process;

void            syscall_test_reset_state(void);
void            syscall_test_init_process_environment(void);
struct process* syscall_test_spawn_process(const char* name);
