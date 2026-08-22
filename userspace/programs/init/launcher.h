#pragma once

#include <base/startup.h>
#include <stdbool.h>

/* Load, start, and detach the initial userspace loader process. */
bool loader_launch(struct init_startup_info* startup);
