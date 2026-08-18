#pragma once

/*
 * Generic immutable binary block interface.
 *
 * A Blob represents a fixed-size immutable sequence of bytes exposed through
 * a capability. It is a common interface that may be implemented by objects from
 * different protocols and services, such as boot modules, files or package contents.
 *
 * The Blob size and contents must remain unchanged for the lifetime of the
 * capability.
 */

#include <base/cap.h>
#include <base/syscall.h>
#include <stddef.h>
#include <stdint.h>

enum blob_op {
	BLOB_OP_INFO = 0u,
	BLOB_OP_READ = 1u,
};

struct blob_request_header {
	uint32_t op;
};

/* Query the total size of a Blob. */
struct blob_info_request {
	struct blob_request_header header;
};

struct blob_info_response {
	uint64_t size;
};

/* Read an exact byte range from a Blob. */
struct blob_read_request {
	struct blob_request_header header;
	uint64_t                   offset;
	uint64_t                   size;
};

/* Read Blob metadata through a Blob capability. */
syscall_status_t blob_get_info(cap_id_t blob_cap, struct blob_info_response* out_info);

/* Copy an exact byte range from a Blob. */
syscall_status_t blob_read(cap_id_t blob_cap, uint64_t offset, void* buffer, size_t size);
