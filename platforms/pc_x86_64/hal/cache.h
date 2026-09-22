#pragma once

#include <stdbool.h>

/* Handle an executable-cache synchronization NMI on the local CPU. */
bool x86_64_cache_handle_sync_nmi(void);
