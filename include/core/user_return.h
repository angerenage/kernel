#pragma once

struct hal_userspace_return_frame;

/*
 * Final architecture-independent boundary before a native frame is restored.
 * Kernel-mode returns are ignored. A userspace return must belong to the
 * current userspace thread and its owning process.
 */
void core_finalize_user_return(struct hal_userspace_return_frame* frame);
