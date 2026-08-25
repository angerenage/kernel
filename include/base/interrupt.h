#pragma once

#include <stdint.h>

/* Platform-defined hardware interrupt source identifier. */
typedef uint64_t interrupt_id_t;

/* No hardware interrupt source uses this sentinel. */
#define INTERRUPT_ID_INVALID UINT64_MAX
