#pragma once

#include <base/cap.h>
#include <base/process.h>
#include <stdint.h>

/* Identity information returned by the self capability. */
struct self_info {
	process_id_t pid;
	uint64_t     thread_id;
	uint64_t     thread_count;
	cap_id_t     self_cap;
};
