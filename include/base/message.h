#pragma once

#include <stddef.h>
#include <stdint.h>

#define MESSAGE_MAX_SIZE 1024u
#define MESSAGE_QUEUE_DEPTH 32u

/* Result codes for message send/receive operations. */
enum message_result {
	MESSAGE_OK = 0,
	MESSAGE_INVALID_ARGUMENTS,
	MESSAGE_INVALID_PID,
	MESSAGE_TOO_LARGE,
	MESSAGE_QUEUE_FULL,
	MESSAGE_NO_MESSAGE,
};
