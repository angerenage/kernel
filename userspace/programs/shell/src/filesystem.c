#include <libc/string.h>
#include <protocol/filesystem.h>
#include <runtime/vfs.h>
#include <stdbool.h>
#include <stddef.h>
#include <system/capability.h>

#include "shell.h"

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

enum shell_change_directory_result shell_change_directory(struct shell* shell, const char* path, size_t path_size) {
	struct filesystem_node_handle node = {.capability = CAP_ID_INVALID};
	struct filesystem_call_result result;
	char                          normalized[SHELL_PATH_CAPACITY];
	size_t                        normalized_size;
	cap_rights_t                  requested_rights = CAP_READ | CAP_WRITE | CAP_DELEGATE;

	if (shell == NULL || shell->vfs_cap == CAP_ID_INVALID ||
	    !shell_resolve_path(shell, path, path_size, normalized, &normalized_size))
		return SHELL_CHANGE_DIRECTORY_INVALID_PATH;
	result = vfs_open(shell->vfs_cap, normalized, normalized_size, requested_rights, &node);
	if (result.transport_status == SYSCALL_STATUS_OK &&
	    (result.status == FILESYSTEM_STATUS_READ_ONLY || result.status == FILESYSTEM_STATUS_ACCESS_DENIED)) {
		if (node.capability != CAP_ID_INVALID) (void)cap_drop(node.capability);
		requested_rights = CAP_READ | CAP_DELEGATE;
		result           = vfs_open(shell->vfs_cap, normalized, normalized_size, requested_rights, &node);
	}
	if (result.transport_status != SYSCALL_STATUS_OK) {
		if (node.capability != CAP_ID_INVALID) (void)cap_drop(node.capability);
		return SHELL_CHANGE_DIRECTORY_FAILED;
	}
	if (result.status != FILESYSTEM_STATUS_OK) {
		if (node.capability != CAP_ID_INVALID) (void)cap_drop(node.capability);
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
	shell->current_directory_cap    = node.capability;
	shell->current_directory_rights = CAP_CALL | CAP_READ;
	if ((requested_rights & CAP_WRITE) != 0u) shell->current_directory_rights |= CAP_WRITE;
	memcpy(shell->current_directory, normalized, normalized_size);
	shell->current_directory_size = normalized_size;
	return SHELL_CHANGE_DIRECTORY_OK;
}
