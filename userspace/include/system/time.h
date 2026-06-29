#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Yield the current thread's remaining scheduler time. */
bool sched_yield(void);

/* Block the calling thread for at least the requested number of milliseconds. */
bool sleep_ms(size_t milliseconds);

/* Read the scheduler's monotonic tick counter. */
bool tick_count(uint64_t* out_ticks);
