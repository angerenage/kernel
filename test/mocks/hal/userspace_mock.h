#pragma once

#include <stdbool.h>

void hal_userspace_mock_set_context_init_result(bool result);
void hal_userspace_mock_set_context_init_hook(void (*hook)(void));
