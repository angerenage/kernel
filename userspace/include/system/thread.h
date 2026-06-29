#pragma once

#include <base/cap.h>
#include <base/thread.h>
#include <stdbool.h>
#include <stdint.h>

/* Wait for a thread to terminate and copy out its exit code. */
bool thread_join(cap_id_t cap, uintptr_t* out_exit_code);

/* Detach a thread so it is reaped automatically at termination. */
bool thread_detach(cap_id_t cap);

/* Request deferred cancellation of a thread. */
bool thread_cancel(cap_id_t cap);

/* Enable or disable deferred cancellation on the target thread. */
bool thread_set_cancel_enabled(cap_id_t cap, bool enabled);

/* Check whether the target thread currently has actionable pending cancellation. */
bool thread_test_cancel(cap_id_t cap, bool* out_should_cancel);
