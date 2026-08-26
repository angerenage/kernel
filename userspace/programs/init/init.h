#pragma once

#include <base/cap.h>
#include <stddef.h>

/* Validated state retained by init after its CRT startup has completed. */
struct init_state {
	cap_id_t kernel_resources_cap;
	cap_id_t serial_cap;
	size_t   page_size;
};

extern struct init_state g_init;
