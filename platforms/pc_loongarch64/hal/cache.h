#pragma once

#include <stdbool.h>

/* Complete a pending executable-cache synchronization request on the current CPU. */
bool loongarch64_cache_handle_sync_ipi(void);
