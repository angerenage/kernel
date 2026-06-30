#pragma once

#include <base/syscall.h>
#include <stddef.h>
#include <stdint.h>

/* Yield the current thread's remaining scheduler time. */
syscall_status_t sched_yield(void);

/* Block the calling thread for at least the requested number of milliseconds. */
syscall_status_t sleep_ms(size_t milliseconds);

/* Read the scheduler's monotonic tick counter. */
syscall_status_t tick_count(uint64_t* out_ticks);
