#pragma once

/* Controller-independent PS/2 byte transport protocol. */

#include <base/cap.h>
#include <stdint.h>

/* Maximum number of ordered bytes returned by one transport read. */
#define PS2_TRANSPORT_MAX_READ 256u

/* Rights exposed by a PS/2 controller on each child transport resource. */
#define PS2_TRANSPORT_DRIVER_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_READ | CAP_WRITE))

/* Generic and identified compatible IDs used by controller-created children. */
#define PS2_COMPATIBLE_DEVICE "ps2:device"
#define PS2_COMPATIBLE_KEYBOARD "ps2:keyboard"
#define PS2_COMPATIBLE_MOUSE "ps2:mouse"

/* Name of the controller-independent capability resource on a PS/2 child. */
#define PS2_TRANSPORT_RESOURCE "transport"

/* Operations accepted by a controller-independent PS/2 byte transport. */
enum ps2_transport_op {
	PS2_TRANSPORT_OP_READ = 0u,
	PS2_TRANSPORT_OP_WRITE,
	PS2_TRANSPORT_OP_ACQUIRE_RECEIVE_SIGNAL,
};

/* Common operation header for PS/2 transport requests. */
struct ps2_transport_request_header {
	enum ps2_transport_op op;
};

/* Request up to max_size immediately available bytes without blocking. */
struct ps2_transport_read_request {
	struct ps2_transport_request_header header;
	uint32_t                            max_size;
};

/* Status bits returned with a successful PS/2 transport read. */
enum ps2_transport_read_flags {
	PS2_TRANSPORT_READ_FLAG_NONE    = 0u,
	PS2_TRANSPORT_READ_FLAG_OVERRUN = 1u << 0u,
};

/* Ordered bytes follow this header in a successful read response. */
struct ps2_transport_read_response {
	uint32_t size;
	uint32_t flags;
	uint8_t  bytes[];
};

/* Write one byte after the controller accepts it into its input register. */
struct ps2_transport_write_request {
	struct ps2_transport_request_header header;
	uint8_t                             value;
	uint8_t                             reserved[3];
};

/* Request the coalesced signal used to announce newly readable state. */
struct ps2_transport_acquire_signal_request {
	struct ps2_transport_request_header header;
};

/* Receive-signal capability delegated to the transport caller with CAP_WAIT. */
struct ps2_transport_acquire_signal_response {
	cap_id_t capability;
};
