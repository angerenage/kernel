#include "root_pivot.h"

#include <protocol/filesystem.h>
#include <protocol/vfs.h>
#include <runtime/filesystem.h>
#include <runtime/vfs.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <system/capability.h>

#define BOOTFS_SERVICE_NAME "boot"
#define FILESYSTEM_PROTOCOL_VERSION_MAJOR 1u
#define FILESYSTEM_PROTOCOL_VERSION_MINOR 0u

static bool call_succeeded(struct filesystem_call_result result) {
	return result.transport_status == SYSCALL_STATUS_OK && result.status == FILESYSTEM_STATUS_OK;
}

static bool mount_bootfs_root(const struct registry_service_handle* vfs, const struct registry_service_handle* bootfs) {
	cap_id_t delegated = CAP_ID_INVALID;
	if (cap_delegate(bootfs->capability, vfs->owner, CAP_CALL | CAP_READ | CAP_DELEGATE, &delegated) !=
	    SYSCALL_STATUS_OK)
		return false;
	struct filesystem_call_result result =
		vfs_directory_mount(vfs->capability, "/", 1u, delegated, VFS_MOUNT_READ_ONLY);
	if (call_succeeded(result)) return true;
	(void)cap_revoke(delegated, 0u);
	return false;
}

bool init_root_pivot(const struct registry_service_handle* vfs, cap_id_t new_root, uint32_t mount_flags) {
	const struct registry_protocol_query bootfs_query = {
		.namespace_path = FILESYSTEM_NAMESPACE,
		.protocol       = FILESYSTEM_PROTOCOL_NAME,
		.major          = FILESYSTEM_PROTOCOL_VERSION_MAJOR,
		.minor          = FILESYSTEM_PROTOCOL_VERSION_MINOR,
	};
	struct registry_service_handle bootfs            = {.capability = CAP_ID_INVALID};
	struct filesystem_node_handle  modules           = {.capability = CAP_ID_INVALID};
	cap_id_t                       delegated_modules = CAP_ID_INVALID;
	bool                           root_unmounted    = false;
	bool                           new_root_mounted  = false;
	bool                           success           = false;

	if (vfs == NULL || vfs->capability == CAP_ID_INVALID || vfs->owner == PROCESS_PID_INVALID ||
	    new_root == CAP_ID_INVALID)
		return false;
	struct registry_call_result acquire = registry_acquire(&bootfs_query, BOOTFS_SERVICE_NAME, &bootfs);
	if (acquire.transport_status != SYSCALL_STATUS_OK || acquire.status != REGISTRY_STATUS_OK) goto cleanup;
	struct filesystem_call_result result =
		filesystem_directory_open(bootfs.capability, "boot", sizeof("boot") - 1u, CAP_READ | CAP_DELEGATE, &modules);
	if (!call_succeeded(result) || modules.info.type != FILESYSTEM_NODE_DIRECTORY) goto cleanup;
	if (cap_delegate(modules.capability, vfs->owner, CAP_CALL | CAP_READ | CAP_DELEGATE, &delegated_modules) !=
	    SYSCALL_STATUS_OK)
		goto cleanup;
	if (!call_succeeded(vfs_directory_unmount(vfs->capability, "/", 1u))) goto cleanup;
	root_unmounted = true;
	result         = vfs_directory_mount(vfs->capability, "/", 1u, new_root, mount_flags);
	if (!call_succeeded(result)) goto rollback;
	new_root_mounted = true;
	result =
		vfs_directory_mount(vfs->capability, "/boot", sizeof("/boot") - 1u, delegated_modules, VFS_MOUNT_READ_ONLY);
	if (!call_succeeded(result)) goto rollback;
	delegated_modules = CAP_ID_INVALID;
	success           = true;
	printf("init: retained bootfs at /boot after root pivot\n");
	goto cleanup;

rollback:
	if (new_root_mounted) {
		if (!call_succeeded(vfs_directory_unmount(vfs->capability, "/", 1u))) goto cleanup;
		new_root_mounted = false;
	}
	if (root_unmounted && !mount_bootfs_root(vfs, &bootfs)) {
		printf("init: failed to restore bootfs after root pivot failure\n");
	}

cleanup:
	if (!success && !new_root_mounted) (void)cap_revoke(new_root, 0u);
	if (delegated_modules != CAP_ID_INVALID) (void)cap_revoke(delegated_modules, 0u);
	if (modules.capability != CAP_ID_INVALID) (void)cap_drop(modules.capability);
	if (bootfs.capability != CAP_ID_INVALID) (void)cap_drop(bootfs.capability);
	return success;
}
