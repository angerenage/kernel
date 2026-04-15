#pragma once

#include <hal/cpu.h>

typedef void (*hal_cpu_mock_context_switch_hook_t)(struct thread_context* current, const struct thread_context* next);

void hal_cpu_mock_set_context_switch_hook(hal_cpu_mock_context_switch_hook_t hook);
