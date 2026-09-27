#pragma once

#include <stdint.h>

/* Operations supported by a record access capability. */
enum record_op {
	RECORD_OP_COUNT = 0,
	RECORD_OP_GET,
};

/* Common prefix for every record access request. */
struct record_request_header {
	enum record_op op; /* Requested record operation. */
};

/* Request the number of records exposed by a capability. */
struct record_count_request {
	struct record_request_header header;
};

/* Number of records exposed by a capability. */
struct record_count_response {
	uint64_t count; /* Number of records currently available. */
};

/* Request one record by its stable discovery index. */
struct record_get_request {
	struct record_request_header header;
	uint64_t                     index; /* Stable discovery index to read. */
};
