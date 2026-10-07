#pragma once

/*
 * Generic sequential byte-stream capability protocol.
 *
 * A Stream has independent read and write cursors. All capability grants for
 * the same Stream object share those cursors, and operations are linearized
 * within each direction. The protocol deliberately exposes neither a total
 * length nor positioning, flushing, or close operations; capability lifetime
 * controls object lifetime.
 *
 * STREAM_OP_READ requires CAP_READ in addition to CAP_CALL. Its response is a
 * raw byte sequence whose response size is the number of bytes read. A short
 * response is successful, and an empty response to a non-empty request means
 * end-of-stream. A provider must block rather than return an empty response
 * merely because data is temporarily unavailable.
 *
 * STREAM_OP_WRITE requires CAP_WRITE in addition to CAP_CALL. The request is
 * followed by exactly size bytes, and the fixed response reports how many were
 * accepted. A successful non-empty write must accept at least one byte.
 *
 * If an operation makes progress before encountering an error, the provider
 * returns a successful short transfer. An error response therefore guarantees
 * that no bytes were transferred and the corresponding cursor did not advance.
 * Zero-length requests are successful no-ops.
 */

#include <base/cap.h>
#include <base/syscall.h>
#include <stddef.h>
#include <stdint.h>

enum stream_op {
	STREAM_OP_READ  = 0u,
	STREAM_OP_WRITE = 1u,
};

struct stream_request_header {
	uint32_t op;
};

/* Read at most size bytes from the current read cursor. */
struct stream_read_request {
	struct stream_request_header header;
	uint32_t                     reserved;
	uint64_t                     size;
};

/* Write request followed by exactly size bytes. */
struct stream_write_request {
	struct stream_request_header header;
	uint32_t                     reserved;
	uint64_t                     size;
	uint8_t                      data[];
};

struct stream_write_response {
	uint64_t size;
};

/*
 * Perform one bounded read operation. The returned count may be smaller than
 * size; zero after a non-empty request means end-of-stream.
 */
syscall_status_t stream_read(cap_id_t stream_cap, void* buffer, size_t size, size_t* out_read);

/* Perform one bounded write operation and return the number of accepted bytes. */
syscall_status_t stream_write(cap_id_t stream_cap, const void* data, size_t size, size_t* out_written);
