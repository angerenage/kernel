#pragma once

#include <runtime/program.h>
#include <stdbool.h>
#include <stddef.h>

#include "init.h"

/* Load, start, and detach one userspace boot module with ordered application capabilities. */
bool bootstrap_launch(const struct init_state* init, const char* module_name, const char* description,
                      size_t capability_count, const struct program_capability_argument capabilities[]);
