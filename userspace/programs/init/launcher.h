#pragma once

#include <stdbool.h>

#include "init.h"

/* Load, start, and detach the initial userspace loader process. */
bool loader_launch(const struct init_state* init);
