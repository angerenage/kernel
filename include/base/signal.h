#pragma once

#include <base/process.h>
#include <base/upcall.h>
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

/* Delivery statistics returned by one Signal publication. */
struct signal_send_response {
	uint64_t receiver_count;
	uint64_t delivery_count;
};

/* Userspace-selectable behavior attached to one Signal publication. */
enum signal_send_flags {
	SIGNAL_SEND_FLAG_NONE     = 0u,
	SIGNAL_SEND_FLAG_COALESCE = 1u << 0,
};

/* Lifecycle properties fixed when an asynchronous Signal handler is registered. */
enum signal_handler_flags {
	SIGNAL_HANDLER_FLAG_NONE    = 0u,
	SIGNAL_HANDLER_FLAG_ONESHOT = 1u << 0,
};

/* Flags returned by SIGNAL_OP_READ. */
enum signal_read_flags {
	SIGNAL_READ_FLAG_NONE      = 0u,
	SIGNAL_READ_FLAG_HAS_VALUE = 1u << 0,
};

/* Control operations routed through the Signal capability with cap_call. */
enum signal_op {
	SIGNAL_OP_UNSUBSCRIBE = 0,
	SIGNAL_OP_DESTROY,
	SIGNAL_OP_SET_HANDLER,
	SIGNAL_OP_CLEAR_HANDLER,
	SIGNAL_OP_READ,
};

/* Common header for all Signal capability requests. */
struct signal_request_header {
	enum signal_op op;
};

/* Request to remove the calling thread's synchronous receiver. */
struct signal_unsubscribe_request {
	struct signal_request_header header;
};

/* Request to destroy the Signal object. */
struct signal_destroy_request {
	struct signal_request_header header;
};

/* Request to register the calling thread's asynchronous upcall handler. */
struct signal_set_handler_request {
	struct signal_request_header header;
	user_upcall_entry_t*         handler;
	uint32_t                     flags;
};

/* Request to remove the calling thread's persistent upcall handler. */
struct signal_clear_handler_request {
	struct signal_request_header header;
};

/* Request a diagnostic snapshot through CAP_CALL | CAP_READ. */
struct signal_read_request {
	struct signal_request_header header;
};

/*
 * Signal state plus queue-wide upcall accounting for the calling uthread.
 * The upcall counters are intentionally caller-scoped: one uthread queue may
 * receive requests from multiple Signals and future upcall origins.
 */
struct signal_read_response {
	uint64_t generation;
	uint64_t handler_count;
	uint64_t wait_subscription_count;
	uint64_t blocked_waiter_count;
	uint64_t caller_upcall_pending_count;
	uint64_t caller_upcall_dropped_count;
	uint64_t caller_upcall_capacity;
	uint64_t flags;
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
	SIGNAL_WAIT_INTERRUPTED,
	SIGNAL_WAIT_RECEIVER_NOT_REGISTERED,
	SIGNAL_HANDLER_ALREADY_REGISTERED,
};
