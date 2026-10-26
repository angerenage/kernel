#pragma once

/*
 * Mount-aware virtual-filesystem capability protocol.
 *
 * The vfs service provides one absolute global namespace. Files use the raw
 * filesystem file protocol; opened directories use the VFS-directory protocol
 * below so mount topology remains private to the VFS.
 *
 * Current specification version: 1.0-beta
 */

#include <protocol/filesystem.h>

#define VFS_NAMESPACE "fs"
#define VFS_PROTOCOL_NAME "vfs"
#define VFS_SERVICE_NAME "root"

enum vfs_op {
	VFS_OP_OPEN             = 0u,
	VFS_OP_CREATE_FILE      = 1u,
	VFS_OP_CREATE_DIRECTORY = 2u,
	VFS_OP_REMOVE           = 3u,
	VFS_OP_RENAME           = 4u,
	VFS_OP_MOUNT            = 5u,
	VFS_OP_UNMOUNT          = 6u,
};

/* Followed by path_size bytes containing one normalized absolute path. */
struct vfs_open_request {
	struct filesystem_request_header header;
	uint32_t                         path_size;
	cap_rights_t                     rights;
	uint8_t                          path[];
};

/* Followed by path_size bytes containing one normalized absolute path. */
struct vfs_path_request {
	struct filesystem_request_header header;
	uint32_t                         path_size;
	uint8_t                          path[];
};

/* Followed by old_path_size bytes and then new_path_size bytes. */
struct vfs_rename_request {
	struct filesystem_request_header header;
	uint32_t                         old_path_size;
	uint32_t                         new_path_size;
	uint32_t                         reserved;
	uint8_t                          paths[];
};

enum vfs_directory_op {
	VFS_DIRECTORY_OP_INFO             = 0u,
	VFS_DIRECTORY_OP_ENUMERATE        = 1u,
	VFS_DIRECTORY_OP_OPEN             = 4u,
	VFS_DIRECTORY_OP_CREATE_FILE      = 5u,
	VFS_DIRECTORY_OP_CREATE_DIRECTORY = 6u,
	VFS_DIRECTORY_OP_REMOVE           = 7u,
	VFS_DIRECTORY_OP_RENAME           = 8u,
};

enum vfs_mount_flag {
	VFS_MOUNT_READ_ONLY = 1u << 0,
};

/* Followed by path_size bytes containing one normalized absolute path. */
struct vfs_mount_request {
	struct filesystem_request_header header;
	uint32_t                         path_size;
	uint32_t                         flags;
	cap_id_t                         root_capability;
	uint8_t                          path[];
};

/*
 * Namespace reads require CAP_CALL | CAP_READ, mutations require CAP_CALL |
 * CAP_WRITE, and mount management requires CAP_CALL | CAP_MANAGE.
 *
 * OPEN, creation, REMOVE, and RENAME use the corresponding raw-filesystem
 * request structures, but resolve their normalized relative paths through the
 * VFS directory. This preserves mount traversal without exposing the
 * directory's absolute namespace path. Requested child rights cannot exceed
 * the directory capability's rights. Path operations on a directory detached
 * by removal or unmount return FILESYSTEM_STATUS_BUSY.
 *
 * MOUNT and UNMOUNT are VFS-service operations selecting a normalized absolute
 * path. root_capability is a raw root-directory capability already delegated
 * to the VFS process. A mount may overlay an existing directory or create a
 * synthetic final path component below an existing directory. The VFS retains
 * the root only when mounting succeeds and drops it on unmount. Covered
 * contents reappear afterward. Existing node capabilities remain valid, and
 * nested mounts make their ancestor mount busy. A mounted directory reports
 * FILESYSTEM_NODE_MOUNT_POINT in its metadata.
 *
 * Namespace paths are normalized absolute paths. '/' names the global root;
 * all other paths use the raw-filesystem component rules. RENAME across mount
 * boundaries returns FILESYSTEM_STATUS_CROSS_FILESYSTEM. A second mount on the
 * same target returns FILESYSTEM_STATUS_ALREADY_EXISTS, and VFS-directory
 * enumeration has the same numeric-offset and mutation semantics as
 * raw-directory enumeration. Malformed framing, invalid sizes, and nonzero
 * reserved fields return SYSCALL_STATUS_BAD_ARGUMENT; missing capability rights
 * return SYSCALL_STATUS_DENIED. The raw-filesystem response and syscall-status
 * conventions otherwise apply unchanged.
 */
