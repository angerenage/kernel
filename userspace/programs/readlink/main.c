#include <base/cap.h>
#include <protocol/vfs.h>
#include <runtime/filesystem.h>
#include <runtime/init.h>
#include <runtime/vfs.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <system/capability.h>

#define VFS_PROTOCOL_VERSION_MAJOR 1u
#define VFS_PROTOCOL_VERSION_MINOR 1u

static const char* status_message(struct filesystem_call_result result) {
	if (result.transport_status == SYSCALL_STATUS_OK && result.status == FILESYSTEM_STATUS_INVALID_ARGUMENT)
		return "not a symbolic link";
	return filesystem_status_message(result);
}

/*
 * Print the target of one link, or its fully resolved path when canonicalize
 * is set. Relative operands are read below the working directory, whose
 * absolute path the VFS does not expose, so canonicalization needs an
 * absolute operand.
 */
static bool print_link(cap_id_t vfs_cap, const char* operand, bool canonicalize) {
	static char                   output[VFS_PATH_MAX];
	struct filesystem_call_result result;
	size_t                        operand_size = strlen(operand);
	size_t                        output_size  = 0u;

	if (operand_size != 0u && operand[0] == '/') {
		if (canonicalize) result = vfs_resolve(vfs_cap, operand, operand_size, output, sizeof(output), &output_size);
		else result = vfs_read_symbolic_link(vfs_cap, operand, operand_size, output, sizeof(output), &output_size);
	}
	else if (canonicalize) {
		fprintf(stderr, "readlink: %s: -f needs an absolute path\n", operand);
		return false;
	}
	else {
		result = vfs_directory_read_symbolic_link(
			filesystem_get_working_directory(), operand, operand_size, output, sizeof(output), &output_size);
	}
	if (result.transport_status != SYSCALL_STATUS_OK || result.status != FILESYSTEM_STATUS_OK) {
		fprintf(stderr, "readlink: %s: %s\n", operand, status_message(result));
		return false;
	}
	return printf("%.*s\n", (int)output_size, output) >= 0;
}

int main(int argc, char** argv, size_t capc, const cap_id_t* capv) {
	static const struct init_protocol_query query = {
		.namespace_path = VFS_NAMESPACE,
		.protocol       = VFS_PROTOCOL_NAME,
		.major          = VFS_PROTOCOL_VERSION_MAJOR,
		.minor          = VFS_PROTOCOL_VERSION_MINOR,
	};
	struct init_service_handle vfs = {.capability = CAP_ID_INVALID};
	struct init_call_result    acquired;
	bool                       canonicalize = false;
	bool                       success      = true;
	int                        index        = 0;

	if (argc < 0 || (argc != 0 && argv == NULL) || capc != 0u || capv != NULL) return 1;
	if (index < argc && strcmp(argv[index], "-f") == 0) {
		canonicalize = true;
		index++;
	}
	if (index < argc && strcmp(argv[index], "--") == 0) index++;
	if (index == argc) {
		fprintf(stderr, "readlink: usage: readlink [-f] PATH...\n");
		return 1;
	}
	acquired = init_acquire(&query, VFS_SERVICE_NAME, &vfs);
	if (acquired.transport_status != SYSCALL_STATUS_OK || acquired.status != INIT_REGISTRY_OK) {
		fprintf(stderr, "readlink: filesystem unavailable\n");
		return 1;
	}
	for (; index < argc; index++) success = print_link(vfs.capability, argv[index], canonicalize) && success;
	(void)cap_drop(vfs.capability);
	return success ? 0 : 1;
}
