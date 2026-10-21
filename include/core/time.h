#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Select a monotonic clock source and capture its arbitrary epoch. Safe to call during boot initialization. */
bool time_monotonic_init(void);

/* Return elapsed nanoseconds since the monotonic clock epoch. Values never decrease. */
bool time_monotonic_now(uint64_t* out_nanoseconds);

/* Return the effective resolution of the selected source in nanoseconds, or 0 when unavailable. */
uint64_t time_monotonic_resolution_ns(void);
