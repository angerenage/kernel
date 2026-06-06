#pragma once

#include <stdint.h>

typedef uintptr_t channel_id_t;

#define CHANNEL_ID_INVALID ((channel_id_t)0u)

/* Result codes for channel creation, send, receive, and destroy operations. */
enum channel_result {
	CHANNEL_OK = 0,
	CHANNEL_INVALID_ARGUMENTS,
	CHANNEL_NOT_OWNER,
	CHANNEL_NOT_FOUND,
	CHANNEL_NO_MESSAGE,
	CHANNEL_BUFFER_TOO_SMALL,
	CHANNEL_QUEUE_FULL,
	CHANNEL_NO_MEMORY,
	CHANNEL_LIMIT_REACHED,
};
