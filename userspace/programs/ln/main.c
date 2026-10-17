#include <base/cap.h>
#include <protocol/vfs.h>
#include <runtime/filesystem.h>
#include <runtime/init.h>
#include <runtime/vfs.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <system/capability.h>

#define VFS_PROTOCOL_VERSION_MAJOR 1u
#define VFS_PROTOCOL_VERSION_MINOR 1u

/* Create the link through the VFS service for an absolute path, or below the working directory otherwise. */
static struct filesystem_call_result create_link(const char* path, const char* target) {
	static const struct init_protocol_query query = {
		.namespace_path = VFS_NAMESPACE,
		.protocol       = VFS_PROTOCOL_NAME,
		.major          = VFS_PROTOCOL_VERSION_MAJOR,
		.minor          = VFS_PROTOCOL_VERSION_MINOR,
	};
	struct init_service_handle    vfs = {.capability = CAP_ID_INVALID};
	struct init_call_result       acquired;
	struct filesystem_call_result result;

	if (path[0] != '/')
		return vfs_directory_create_symbolic_link(
			filesystem_get_working_directory(), path, strlen(path), target, strlen(target));
	acquired = init_acquire(&query, VFS_SERVICE_NAME, &vfs);
	if (acquired.transport_status != SYSCALL_STATUS_OK || acquired.status != INIT_REGISTRY_OK)
		return (struct filesystem_call_result){.status           = FILESYSTEM_STATUS_NONE,
		                                       .transport_status = SYSCALL_STATUS_FAILED};
	result = vfs_create_symbolic_link(vfs.capability, path, strlen(path), target, strlen(target));
	(void)cap_drop(vfs.capability);
	return result;
}

int main(int argc, char** argv, size_t capc, const cap_id_t* capv) {
	struct filesystem_call_result result;
	int                           index = 0;

	if (argc < 0 || (argc != 0 && argv == NULL) || capc != 0u || capv != NULL) return 1;
	if (index < argc && strcmp(argv[index], "-s") == 0) index++;
	else {
		fprintf(stderr, "ln: only symbolic links are supported, use ln -s TARGET LINK\n");
		return 1;
	}
	if (index < argc && strcmp(argv[index], "--") == 0) index++;
	if (argc - index != 2) {
		fprintf(stderr, "ln: usage: ln -s TARGET LINK\n");
		return 1;
	}
	result = create_link(argv[index + 1], argv[index]);
	if (result.transport_status == SYSCALL_STATUS_OK && result.status == FILESYSTEM_STATUS_OK) return 0;
	fprintf(stderr, "ln: %s: %s\n", argv[index + 1], filesystem_status_message(result));
	return 1;
}
