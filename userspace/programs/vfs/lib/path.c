#include <runtime/filesystem.h>
#include <runtime/vfs.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool component_valid(const uint8_t* component, size_t size) {
	if (size == 0u || size > FILESYSTEM_NAME_MAX) return false;
	for (size_t index = 0u; index < size; index++)
		if (component[index] == '/') return false;
	return true;
}

bool vfs_relative_path_valid(const void* path, size_t path_size) {
	const uint8_t* bytes = path;
	size_t         start = 0u;

	if (path == NULL || path_size == 0u || bytes[0u] == '/' || bytes[path_size - 1u] == '/') return false;
	for (size_t index = 0u; index < path_size; index++) {
		if (bytes[index] != '/') continue;
		if (!component_valid(bytes + start, index - start)) return false;
		start = index + 1u;
	}
	return component_valid(bytes + start, path_size - start);
}

static void remove_component(uint8_t* path, size_t* size) {
	if (*size == 1u) return;
	while (*size > 1u && path[*size - 1u] != '/') (*size)--;
	if (*size > 1u) (*size)--;
}

bool vfs_resolve_directory_path(const void* directory, size_t directory_size, const void* relative,
                                size_t relative_size, void* output, size_t output_capacity, size_t* out_size) {
	const uint8_t* base  = directory;
	const uint8_t* input = relative;
	uint8_t*       path  = output;
	size_t         size;
	size_t         offset = 0u;

	if (directory == NULL || directory_size == 0u || base[0u] != '/' ||
	    (directory_size != 1u && !filesystem_relative_path_valid(base + 1u, directory_size - 1u)) ||
	    !vfs_relative_path_valid(relative, relative_size) || output == NULL || output_capacity < directory_size ||
	    out_size == NULL)
		return false;
	memcpy(path, base, directory_size);
	size = directory_size;
	while (offset < relative_size) {
		size_t start = offset;
		while (offset < relative_size && input[offset] != '/') offset++;
		size_t component_size = offset - start;
		if (component_size == 1u && input[start] == '.') {
			offset++;
			continue;
		}
		if (component_size == 2u && input[start] == '.' && input[start + 1u] == '.') {
			remove_component(path, &size);
			offset++;
			continue;
		}
		size_t separator_size = size == 1u ? 0u : 1u;
		if (separator_size + component_size > output_capacity - size) return false;
		if (separator_size != 0u) path[size++] = '/';
		memcpy(path + size, input + start, component_size);
		size += component_size;
		offset++;
	}
	*out_size = size;
	return true;
}
