#pragma once

#include <stdbool.h>
#include <stddef.h>

bool kernel_cmdline_option_enabled(const char* name);
bool kernel_cmdline_option_value(const char* name, const char** value, size_t* value_len);
bool kernel_cmdline_value_equals(const char* value, size_t value_len, const char* expected);
