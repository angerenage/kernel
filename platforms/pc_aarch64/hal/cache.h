#pragma once

/* Complete any pending executable-cache synchronization request on the local CPU. */
void aarch64_cache_poll_sync(void);
