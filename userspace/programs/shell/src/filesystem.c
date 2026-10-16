#include <libc/stdlib.h>
#include <libc/string.h>
#include <protocol/filesystem.h>
#include <protocol/vfs.h>
#include <runtime/init.h>
#include <runtime/vfs.h>
#include <stdbool.h>
#include <stddef.h>
#include <system/capability.h>

#include "shell.h"

#define VFS_PROTOCOL_VERSION_MAJOR 1u
#define VFS_PROTOCOL_VERSION_MINOR 0u

static bool append_component(char* output, size_t* output_size, const char* component, size_t component_size) {
	size_t separator_size = *output_size == 1u ? 0u : 1u;

	if (component_size > FILESYSTEM_NAME_MAX || separator_size + component_size > SHELL_PATH_CAPACITY - *output_size)
		return false;
	if (separator_size != 0u) output[(*output_size)++] = '/';
	memcpy(output + *output_size, component, component_size);
	*output_size += component_size;
	return true;
}

static void remove_component(char* output, size_t* output_size) {
	if (*output_size == 1u) return;
	while (*output_size > 1u && output[*output_size - 1u] != '/') (*output_size)--;
	if (*output_size > 1u) (*output_size)--;
}

bool shell_resolve_path(const struct shell* shell, const char* path, size_t path_size, char* output, size_t* out_size) {
	size_t output_size;
	size_t offset = 0u;

	if (shell == NULL || path == NULL || path_size == 0u || output == NULL || out_size == NULL) return false;
	if (path[0] == '/') {
		output[0]   = '/';
		output_size = 1u;
	}
	else {
		if (shell->current_directory_size == 0u || shell->current_directory_size > SHELL_PATH_CAPACITY) return false;
		memcpy(output, shell->current_directory, shell->current_directory_size);
		output_size = shell->current_directory_size;
	}
	while (offset < path_size) {
		size_t component_start;
		size_t component_size;

		while (offset < path_size && path[offset] == '/') offset++;
		component_start = offset;
		while (offset < path_size && path[offset] != '/') offset++;
		component_size = offset - component_start;
		if (component_size == 0u || (component_size == 1u && path[component_start] == '.')) continue;
		if (component_size == 2u && path[component_start] == '.' && path[component_start + 1u] == '.') {
			remove_component(output, &output_size);
			continue;
		}
		if (!append_component(output, &output_size, path + component_start, component_size)) return false;
	}
	*out_size = output_size;
	return true;
}

bool shell_initialize(struct shell* shell) {
	static const struct init_protocol_query query = {
		.namespace_path = VFS_NAMESPACE,
		.protocol       = VFS_PROTOCOL_NAME,
		.major          = VFS_PROTOCOL_VERSION_MAJOR,
		.minor          = VFS_PROTOCOL_VERSION_MINOR,
	};
	struct init_service_handle    service = {.capability = CAP_ID_INVALID};
	struct filesystem_node_handle node    = {.capability = CAP_ID_INVALID};
	struct init_call_result       acquired;
	struct filesystem_call_result opened;

	if (shell == NULL) return false;
	*shell   = (struct shell){.vfs_cap = CAP_ID_INVALID, .current_directory_cap = CAP_ID_INVALID};
	acquired = init_acquire(&query, VFS_SERVICE_NAME, &service);
	if (acquired.transport_status != SYSCALL_STATUS_OK || acquired.status != INIT_REGISTRY_OK ||
	    service.capability == CAP_ID_INVALID)
		return false;
	opened = vfs_open(service.capability, "/", 1u, CAP_READ | CAP_DELEGATE, &node);
	if (opened.transport_status != SYSCALL_STATUS_OK || opened.status != FILESYSTEM_STATUS_OK ||
	    node.info.type != FILESYSTEM_NODE_DIRECTORY || node.capability == CAP_ID_INVALID) {
		if (node.capability != CAP_ID_INVALID) (void)cap_drop(node.capability);
		(void)cap_drop(service.capability);
		return false;
	}
	shell->vfs_cap                = service.capability;
	shell->current_directory_cap  = node.capability;
	shell->current_directory[0]   = '/';
	shell->current_directory_size = 1u;
	return true;
}

bool shell_destroy(struct shell* shell) {
	bool success = true;

	if (shell == NULL) return false;
	for (size_t i = 0u; i < shell->history_count; i++) free(shell->history[i].line);
	shell->history_count = 0u;
	if (shell->current_directory_cap != CAP_ID_INVALID && cap_drop(shell->current_directory_cap) != SYSCALL_STATUS_OK)
		success = false;
	shell->current_directory_cap = CAP_ID_INVALID;
	if (shell->vfs_cap != CAP_ID_INVALID && cap_drop(shell->vfs_cap) != SYSCALL_STATUS_OK) success = false;
	shell->vfs_cap = CAP_ID_INVALID;
	return success;
}

enum shell_change_directory_result shell_change_directory(struct shell* shell, const char* path, size_t path_size) {
	struct filesystem_node_handle node = {.capability = CAP_ID_INVALID};
	struct filesystem_call_result result;
	char                          normalized[SHELL_PATH_CAPACITY];
	size_t                        normalized_size;

	if (shell == NULL || shell->vfs_cap == CAP_ID_INVALID ||
	    !shell_resolve_path(shell, path, path_size, normalized, &normalized_size))
		return SHELL_CHANGE_DIRECTORY_INVALID_PATH;
	result = vfs_open(shell->vfs_cap, normalized, normalized_size, CAP_READ | CAP_DELEGATE, &node);
	if (result.transport_status != SYSCALL_STATUS_OK) return SHELL_CHANGE_DIRECTORY_FAILED;
	if (result.status != FILESYSTEM_STATUS_OK) {
		switch (result.status) {
		case FILESYSTEM_STATUS_NOT_FOUND:
			return SHELL_CHANGE_DIRECTORY_NOT_FOUND;
		case FILESYSTEM_STATUS_NOT_DIRECTORY:
			return SHELL_CHANGE_DIRECTORY_NOT_DIRECTORY;
		case FILESYSTEM_STATUS_ACCESS_DENIED:
			return SHELL_CHANGE_DIRECTORY_DENIED;
		case FILESYSTEM_STATUS_INVALID_ARGUMENT:
			return SHELL_CHANGE_DIRECTORY_INVALID_PATH;
		default:
			return SHELL_CHANGE_DIRECTORY_FAILED;
		}
	}
	if (node.info.type != FILESYSTEM_NODE_DIRECTORY) {
		if (node.capability != CAP_ID_INVALID) (void)cap_drop(node.capability);
		return SHELL_CHANGE_DIRECTORY_NOT_DIRECTORY;
	}
	if (node.capability == CAP_ID_INVALID) return SHELL_CHANGE_DIRECTORY_FAILED;
	if (shell->current_directory_cap != CAP_ID_INVALID && cap_drop(shell->current_directory_cap) != SYSCALL_STATUS_OK) {
		(void)cap_drop(node.capability);
		return SHELL_CHANGE_DIRECTORY_FAILED;
	}
	shell->current_directory_cap = node.capability;
	memcpy(shell->current_directory, normalized, normalized_size);
	shell->current_directory_size = normalized_size;
	return SHELL_CHANGE_DIRECTORY_OK;
}
