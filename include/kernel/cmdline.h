#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Return true when the kernel command line defines name as a boolean-true option. */
bool kernel_cmdline_option_enabled(const char* name);

/* Return the value associated with name in the kernel command line, when present. */
bool kernel_cmdline_option_value(const char* name, const char** value, size_t* value_len);

/* Return true when value (length value_len) matches expected, treating NULL safely. */
bool kernel_cmdline_value_equals(const char* value, size_t value_len, const char* expected);
