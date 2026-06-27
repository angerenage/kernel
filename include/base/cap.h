#pragma once

#include <base/process.h>
#include <stddef.h>
#include <stdint.h>

typedef uint64_t cap_id_t;
typedef uint64_t cap_rights_t;
typedef uint64_t cap_call_id_t;

#define CAP_ID_INVALID ((cap_id_t)0u)
#define CAP_CALL_ID_INVALID ((cap_call_id_t)0u)
#define CAP_MAX_REQUEST_SIZE (64 * 1024)
#define CAP_MAX_RESPONSE_SIZE (64 * 1024)
#define CAP_REQUEST_QUEUE_DEPTH 16u

enum cap_right {
	CAP_READ     = 1ull << 0,
	CAP_WRITE    = 1ull << 1,
	CAP_EXEC     = 1ull << 2,
	CAP_MAP      = 1ull << 3,
	CAP_CALL     = 1ull << 4,
	CAP_WAIT     = 1ull << 5,
	CAP_SIGNAL   = 1ull << 6,
	CAP_ALLOCATE = 1ull << 7,
	CAP_DESTROY  = 1ull << 8,

	CAP_DELEGATE = 1ull << 16,
	CAP_DERIVE   = 1ull << 17,
	CAP_REVOKE   = 1ull << 18,
	CAP_MANAGE   = 1ull << 19,
};

/* Request delivered to a server endpoint when a client issues cap_call. Userspace servers receive response == NULL
 *
 * and complete call_id with cap_reply; kernel handlers receive a writable response buffer directly. */
struct cap_request {
	cap_call_id_t call_id;
	process_id_t  caller;
	cap_id_t      cap_id;
	uint64_t      object_id;
	cap_rights_t  rights;
	const void*   request;
	size_t        request_size;
	void*         response;
	size_t        response_capacity;
};
