#pragma once

#include <stdbool.h>

#include "init.h"

/* Load, start, and detach one userspace boot module with the kernel loader. */
bool bootstrap_launch(const struct init_state* init, const char* module_name, const char* description);
