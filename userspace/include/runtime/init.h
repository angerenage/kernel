#pragma once

#include <base/cap.h>
#include <stdint.h>

/* Init service capability installed from process_startup_info by _start. */
extern cap_id_t init_cap_id;

#define INIT_SERVICE_OBJECT_ID 1u

enum init_signal_cookie {
	INIT_SIGNAL_WAIT_COOKIE   = 0x57414954u,
	INIT_SIGNAL_UPCALL_COOKIE = 0x5550434cu,
};

enum init_op {
	INIT_OP_PING = 0,
	INIT_OP_GET_SIGNAL,
	INIT_OP_SIGNAL_BACK,
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

struct init_get_signal_request {
	struct init_request_header header;
};

struct init_get_signal_response {
	cap_id_t     signal_cap;
	process_id_t init_pid;
};

struct init_signal_back_request {
	struct init_request_header header;
	cap_id_t                   signal_cap;
};

struct init_stop_request {
	struct init_request_header header;
};
