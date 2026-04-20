#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Convert milliseconds to ticks, rounding partial ticks up and saturating at UINT64_MAX. */
bool time_ms_to_ticks(uint64_t ms, uint32_t hz, uint64_t* out_ticks);

/* Convert a millisecond timeout into an absolute future tick deadline. */
bool time_tick_deadline_from_ms(uint64_t current_tick, uint64_t timeout_ms, uint32_t hz, uint64_t* out_deadline_tick);
