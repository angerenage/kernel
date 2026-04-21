#pragma once

#include <hal/cpu.h>
#include <stddef.h>

typedef void (*hal_cpu_mock_context_switch_hook_t)(struct thread_context* current, const struct thread_context* next);

void   hal_cpu_mock_set_context_switch_hook(hal_cpu_mock_context_switch_hook_t hook);
void   hal_cpu_mock_set_thread_context_init_result(bool result);
void   hal_cpu_mock_reset_kicks(void);
size_t hal_cpu_mock_kick_count(const struct cpu* cpu);
