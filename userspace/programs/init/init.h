#pragma once

#include <base/cap.h>

/* Validated state retained by init after its CRT startup has completed. */
struct init_state {
	cap_id_t kernel_resources_cap;
	cap_id_t serial_cap; /* Raw kernel serial capability, private to init. */
	cap_id_t serial_stream_cap;
	cap_id_t memory_allocator_cap;
};

extern struct init_state g_init;
