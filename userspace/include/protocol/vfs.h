#pragma once

/*
 * Mount-aware virtual-filesystem capability protocol.
 *
 * The vfs service provides one absolute global namespace. Files use the raw
 * filesystem file protocol; opened directories use the VFS-directory protocol
 * below so mount topology remains private to the VFS. The VFS is the only
 * component which follows symbolic links.
 *
 * Current specification version: 1.1-beta
 */

#include <protocol/filesystem.h>

#define VFS_NAMESPACE "fs"
#define VFS_PROTOCOL_NAME "vfs"
#define VFS_SERVICE_NAME "root"

enum {
	VFS_PATH_MAX                 = 4096u,
	VFS_SYMBOLIC_LINK_FOLLOW_MAX = 40u,
};

enum vfs_op {
	VFS_OP_OPEN                 = 0u,
	VFS_OP_CREATE_FILE          = 1u,
	VFS_OP_CREATE_DIRECTORY     = 2u,
	VFS_OP_REMOVE               = 3u,
	VFS_OP_RENAME               = 4u,
	VFS_OP_LOOKUP               = 5u,
	VFS_OP_CREATE_SYMBOLIC_LINK = 6u,
	VFS_OP_READ_SYMBOLIC_LINK   = 7u,
	VFS_OP_RESOLVE              = 8u,
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

/* Followed by path_size bytes containing one normalized absolute path and then target_size target bytes. */
struct vfs_symbolic_link_request {
	struct filesystem_request_header header;
	uint32_t                         path_size;
	uint32_t                         target_size;
	uint32_t                         reserved;
	uint8_t                          data[];
};

/* A successful RESOLVE response is followed by one normalized absolute path. */
struct vfs_path_response {
	struct filesystem_response_header header;
	uint8_t                           path[];
};

enum vfs_directory_op {
	VFS_DIRECTORY_OP_INFO                 = 0u,
	VFS_DIRECTORY_OP_ENUMERATE            = 1u,
	VFS_DIRECTORY_OP_MOUNT                = 2u,
	VFS_DIRECTORY_OP_UNMOUNT              = 3u,
	VFS_DIRECTORY_OP_OPEN                 = 4u,
	VFS_DIRECTORY_OP_CREATE_FILE          = 5u,
	VFS_DIRECTORY_OP_CREATE_DIRECTORY     = 6u,
	VFS_DIRECTORY_OP_REMOVE               = 7u,
	VFS_DIRECTORY_OP_RENAME               = 8u,
	VFS_DIRECTORY_OP_LOOKUP               = 9u,
	VFS_DIRECTORY_OP_CREATE_SYMBOLIC_LINK = 10u,
	VFS_DIRECTORY_OP_READ_SYMBOLIC_LINK   = 11u,
};

enum vfs_mount_flag {
	VFS_MOUNT_READ_ONLY = 1u << 0,
};

struct vfs_directory_mount_request {
	struct filesystem_request_header header;
	uint32_t                         flags;
	cap_id_t                         root_capability;
};

/*
 * Namespace reads require CAP_CALL | CAP_READ, mutations require CAP_CALL |
 * CAP_WRITE, and mount management requires CAP_CALL | CAP_MANAGE.
 *
 * OPEN, creation, REMOVE, RENAME, LOOKUP, CREATE_SYMBOLIC_LINK and
 * READ_SYMBOLIC_LINK use the corresponding raw-filesystem request structures,
 * but resolve their normalized relative paths through the VFS directory. This
 * preserves mount traversal without exposing the directory's absolute
 * namespace path. Requested child rights cannot exceed the directory
 * capability's rights. Path operations on a directory detached by removal or
 * unmount return FILESYSTEM_STATUS_BUSY.
 *
 * MOUNT is called on the target VFS-directory capability. root_capability is a
 * raw root-directory capability already delegated to the VFS process. The VFS
 * retains it only when the domain result is FILESYSTEM_STATUS_OK, and drops it
 * on UNMOUNT. The mount overlays covered contents, they reappear on unmount.
 * Existing node capabilities remain valid, and nested mounts make their
 * ancestor mount busy. A mounted directory reports FILESYSTEM_NODE_MOUNT_POINT
 * in its metadata.
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
 *
 * OPEN follows symbolic links in every component. LOOKUP, READ_SYMBOLIC_LINK,
 * CREATE_SYMBOLIC_LINK, creation, REMOVE and RENAME follow links in every
 * component but the final one, which they act on directly. LOOKUP and
 * READ_SYMBOLIC_LINK use vfs_path_request and the raw LOOKUP and
 * READ_SYMBOLIC_LINK responses. RESOLVE uses vfs_path_request, follows every
 * component, and answers with vfs_path_response naming the existing node.
 *
 * A relative link target is interpreted from the directory holding the link;
 * an absolute target restarts at '/' and may cross mounts. '.' and '..' in
 * targets are resolved physically, and '..' at '/' stays at '/'. Request
 * paths never contain '.' or '..', so clients resolve those lexically.
 * Following more than VFS_SYMBOLIC_LINK_FOLLOW_MAX links returns
 * FILESYSTEM_STATUS_LOOP, and a resolved path longer than VFS_PATH_MAX bytes
 * returns FILESYSTEM_STATUS_INVALID_ARGUMENT.
 */
