#pragma once

#include <stdint.h>

typedef uint64_t cap_id_t;
typedef uint64_t cap_rights_t;

#define CAP_ID_INVALID ((cap_id_t)0u)
#define CAP_MAX_REQUEST_SIZE (64 * 1024)

enum cap_right {
	CAP_READ   = 1ull << 0,
	CAP_WRITE  = 1ull << 1,
	CAP_EXEC   = 1ull << 2,
	CAP_MAP    = 1ull << 3,
	CAP_CALL   = 1ull << 4,
	CAP_WAIT   = 1ull << 5,
	CAP_SIGNAL = 1ull << 6,

	CAP_DELEGATE = 1ull << 16,
	CAP_DERIVE   = 1ull << 17,
	CAP_REVOKE   = 1ull << 18,
	CAP_MANAGE   = 1ull << 19,
};
