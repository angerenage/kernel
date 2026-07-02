#pragma once

#include <base/cap.h>
#include <stdint.h>

/* Init service capability installed from process_startup_info by _start. */
extern cap_id_t init_cap_id;

#define INIT_SERVICE_OBJECT_ID 1u

enum init_op {
	INIT_OP_PING = 0,
	INIT_OP_STOP,
};

struct init_request_header {
	enum init_op op;
};

struct init_ping_request {
	struct init_request_header header;
	uint64_t                   value;
};

struct init_ping_response {
	uint64_t value;
};

struct init_stop_request {
	struct init_request_header header;
};
