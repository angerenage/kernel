#pragma once

/*
 * Generic raw-filesystem capability protocol.
 *
 * Raw filesystems expose files and directories but do not interpret mounts.
 * File I/O is positional, directory paths are normalized relative paths, and
 * capability lifetime controls the lifetime of opened nodes.
 *
 * Current specification version: 1.0-beta
 */

#include <base/cap.h>
#include <stdint.h>

#define FILESYSTEM_NAMESPACE "fs"
#define FILESYSTEM_PROTOCOL_NAME "filesystem"
#define FILESYSTEM_PROTOCOL_VERSION_MAJOR 1u
#define FILESYSTEM_PROTOCOL_VERSION_MINOR 0u

enum {
	FILESYSTEM_NAME_MAX = 255u,
};

enum filesystem_status {
	FILESYSTEM_STATUS_OK = 0u,
	FILESYSTEM_STATUS_INVALID_ARGUMENT,
	FILESYSTEM_STATUS_NOT_FOUND,
	FILESYSTEM_STATUS_ALREADY_EXISTS,
	FILESYSTEM_STATUS_NOT_DIRECTORY,
	FILESYSTEM_STATUS_IS_DIRECTORY,
	FILESYSTEM_STATUS_DIRECTORY_NOT_EMPTY,
	FILESYSTEM_STATUS_READ_ONLY,
	FILESYSTEM_STATUS_ACCESS_DENIED,
	FILESYSTEM_STATUS_NO_SPACE,
	FILESYSTEM_STATUS_CROSS_FILESYSTEM,
	FILESYSTEM_STATUS_BUSY,
	FILESYSTEM_STATUS_UNSUPPORTED,
	FILESYSTEM_STATUS_IO_ERROR,
	FILESYSTEM_STATUS_COUNT,

	/* Helper-only sentinel: no domain response was received. */
	FILESYSTEM_STATUS_NONE = UINT32_MAX,
};

enum filesystem_node_type {
	FILESYSTEM_NODE_INVALID   = 0u,
	FILESYSTEM_NODE_FILE      = 1u,
	FILESYSTEM_NODE_DIRECTORY = 2u,
};

enum filesystem_node_flag {
	FILESYSTEM_NODE_READ_ONLY   = 1u << 0,
	FILESYSTEM_NODE_MOUNT_POINT = 1u << 1,
};

/*
 * Rights which may be requested for a node returned by OPEN or creation.
 * CAP_CALL is implicit; a provider adds it to the requested rights.
 */
#define FILESYSTEM_NODE_REQUEST_RIGHTS ((cap_rights_t)(CAP_READ | CAP_WRITE | CAP_MANAGE | CAP_DELEGATE))

struct filesystem_request_header {
	uint32_t op;
};

/* Every well-framed operation starts its response with this domain status. */
struct filesystem_response_header {
	uint32_t status;
	uint32_t reserved;
};

struct filesystem_node_info {
	uint32_t type;
	uint32_t flags;
	uint64_t size;
};

struct filesystem_info_request {
	struct filesystem_request_header header;
	uint32_t                         reserved;
};

struct filesystem_info_response {
	struct filesystem_response_header header;
	struct filesystem_node_info       info;
};

struct filesystem_open_response {
	struct filesystem_response_header header;
	struct filesystem_node_info       info;
	cap_id_t                          capability;
};

enum filesystem_file_op {
	FILESYSTEM_FILE_OP_INFO   = 0u,
	FILESYSTEM_FILE_OP_READ   = 1u,
	FILESYSTEM_FILE_OP_WRITE  = 2u,
	FILESYSTEM_FILE_OP_RESIZE = 3u,
};

struct filesystem_file_read_request {
	struct filesystem_request_header header;
	uint32_t                         reserved;
	uint64_t                         offset;
	uint64_t                         size;
};

/* A successful response is followed by at most size bytes. */
struct filesystem_file_read_response {
	struct filesystem_response_header header;
	uint8_t                           data[];
};

/* The request is followed by exactly size bytes. */
struct filesystem_file_write_request {
	struct filesystem_request_header header;
	uint32_t                         reserved;
	uint64_t                         offset;
	uint64_t                         size;
	uint8_t                          data[];
};

struct filesystem_file_write_response {
	struct filesystem_response_header header;
	uint64_t                          size;
};

struct filesystem_file_resize_request {
	struct filesystem_request_header header;
	uint32_t                         reserved;
	uint64_t                         size;
};

enum filesystem_directory_op {
	FILESYSTEM_DIRECTORY_OP_INFO             = 0u,
	FILESYSTEM_DIRECTORY_OP_OPEN             = 1u,
	FILESYSTEM_DIRECTORY_OP_ENUMERATE        = 2u,
	FILESYSTEM_DIRECTORY_OP_CREATE_FILE      = 3u,
	FILESYSTEM_DIRECTORY_OP_CREATE_DIRECTORY = 4u,
	FILESYSTEM_DIRECTORY_OP_REMOVE           = 5u,
	FILESYSTEM_DIRECTORY_OP_RENAME           = 6u,
};

/* Followed by path_size bytes containing one normalized relative path. */
struct filesystem_directory_open_request {
	struct filesystem_request_header header;
	uint32_t                         path_size;
	cap_rights_t                     rights;
	uint8_t                          path[];
};

struct filesystem_directory_enumerate_request {
	struct filesystem_request_header header;
	uint32_t                         reserved;
	uint64_t                         offset;
	uint64_t                         count;
};

/* Names are length-delimited; bytes after name_size have no meaning. */
struct filesystem_directory_entry {
	uint32_t                    name_size;
	uint32_t                    reserved;
	struct filesystem_node_info info;
	uint8_t                     name[FILESYSTEM_NAME_MAX + 1u];
};

struct filesystem_directory_enumerate_response {
	struct filesystem_response_header header;
	uint64_t                          total;
	uint64_t                          returned;
	struct filesystem_directory_entry entries[];
};

/* Followed by path_size bytes containing one normalized relative path. */
struct filesystem_directory_path_request {
	struct filesystem_request_header header;
	uint32_t                         path_size;
	uint8_t                          path[];
};

/* Followed by old_path_size bytes and then new_path_size bytes. */
struct filesystem_directory_rename_request {
	struct filesystem_request_header header;
	uint32_t                         old_path_size;
	uint32_t                         new_path_size;
	uint32_t                         reserved;
	uint8_t                          paths[];
};

/*
 * Calls require CAP_CALL. INFO, READ, OPEN and ENUMERATE additionally require
 * CAP_READ; WRITE, RESIZE, creation, REMOVE and RENAME require CAP_WRITE.
 *
 * A domain error response consists only of filesystem_response_header. A
 * successful response has the complete operation-specific shape above. A
 * successful non-empty WRITE reports at least one byte; any domain or syscall
 * error transfers no bytes. Enlarging a file (including a write beyond EOF)
 * zero-fills the gap, while shrinking discards trailing bytes. READ may return
 * fewer bytes than requested, and returns no data at EOF.
 *
 * Enumeration offsets are zero-based. Ordering remains stable only while the
 * directory is unchanged; enumeration is not a snapshot. OPEN, creation and
 * RENAME never replace an existing destination. REMOVE accepts files and
 * empty directories. A raw-filesystem RENAME is atomic, or reports
 * FILESYSTEM_STATUS_CROSS_FILESYSTEM when the provider cannot perform it.
 *
 * Relative paths contain no leading or trailing '/', empty component, '.' or
 * '..'. Each component is at most FILESYSTEM_NAME_MAX bytes. Path bytes are
 * length-delimited and are not NUL-terminated on the wire. Malformed framing,
 * unknown operation numbers, invalid sizes, and nonzero reserved fields return
 * SYSCALL_STATUS_BAD_ARGUMENT; missing capability rights return
 * SYSCALL_STATUS_DENIED. Otherwise the syscall status is SYSCALL_STATUS_OK and
 * the response carries a filesystem_status. Transport and provider failures
 * may instead retain their syscall-level failure status without a domain
 * response. A recognized operation which a provider cannot implement returns
 * FILESYSTEM_STATUS_UNSUPPORTED.
 *
 * Raw providers give the VFS brokerable node capabilities. The VFS delegates
 * CAP_CALL plus only the rights requested by its client.
 */
