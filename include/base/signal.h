#pragma once

#include <base/process.h>
#include <stdint.h>

typedef uint64_t signal_id_t;

#define SIGNAL_ID_INVALID ((signal_id_t)0u)
/* Reserved sender for publications produced directly by the kernel. */
#define SIGNAL_SENDER_KERNEL ((process_id_t)0u)

enum {
	SIGNAL_ARGUMENT_COUNT = 4u,
};

/* Four-word value transported by one signal publication. */
struct signal_payload {
	uint64_t args[SIGNAL_ARGUMENT_COUNT];
};

/* One authenticated signal publication as observed by synchronous receivers. */
struct signal_message {
	process_id_t          sender;
	struct signal_payload payload;
};

enum signal_result {
	SIGNAL_OK = 0,
	SIGNAL_INVALID_ARGUMENTS,
	SIGNAL_NOT_FOUND,
	SIGNAL_CLOSED,
	SIGNAL_NO_VALUE,
	SIGNAL_WOULD_BLOCK,
	SIGNAL_HANDLER_NOT_REGISTERED,
	SIGNAL_UNAVAILABLE,
	SIGNAL_NO_MEMORY,
	SIGNAL_WAIT_FAILED,
	SIGNAL_WAIT_CANCELED,
};
