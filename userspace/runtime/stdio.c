#include <base/cap.h>
#include <base/syscall.h>
#include <libc/stdio.h>
#include <libc/stdlib.h>
#include <libc/string.h>
#include <protocol/filesystem.h>
#include <protocol/vfs.h>
#include <runtime/filesystem.h>
#include <runtime/init.h>
#include <runtime/vfs.h>
#include <stdbool.h>
#include <stdint.h>
#include <system/capability.h>
#include <system/display.h>

enum file_kind {
	FILE_KIND_STANDARD_INPUT,
	FILE_KIND_STANDARD_OUTPUT,
	FILE_KIND_STANDARD_ERROR,
	FILE_KIND_VFS,
};

struct libc_file {
	enum file_kind kind;
	cap_id_t       capability;
	uint64_t       offset;
	bool           readable;
	bool           writable;
	bool           end_of_file;
	bool           error;
	bool           locked;
};

struct file_mode {
	bool readable;
	bool writable;
	bool create;
	bool truncate;
	bool append;
};

static FILE standard_input = {
	.kind       = FILE_KIND_STANDARD_INPUT,
	.capability = CAP_ID_INVALID,
	.readable   = true,
};
static FILE standard_output = {
	.kind       = FILE_KIND_STANDARD_OUTPUT,
	.capability = CAP_ID_INVALID,
	.writable   = true,
};
static FILE standard_error = {
	.kind       = FILE_KIND_STANDARD_ERROR,
	.capability = CAP_ID_INVALID,
	.writable   = true,
};

FILE* stdin  = &standard_input;
FILE* stdout = &standard_output;
FILE* stderr = &standard_error;

static void file_lock(FILE* stream) {
	while (__atomic_test_and_set(&stream->locked, __ATOMIC_ACQUIRE)) __asm__ volatile("" ::: "memory");
}

static void file_unlock(FILE* stream) {
	__atomic_clear(&stream->locked, __ATOMIC_RELEASE);
}

static bool call_succeeded(struct filesystem_call_result result) {
	return result.transport_status == SYSCALL_STATUS_OK && result.status == FILESYSTEM_STATUS_OK;
}

static bool file_mode_parse(const char* mode, struct file_mode* out_mode) {
	bool binary = false;
	bool update = false;
	char initial;

	if (mode == NULL || out_mode == NULL) return false;
	initial = mode[0];
	if (initial != 'r' && initial != 'w' && initial != 'a') return false;
	for (size_t index = 1u; mode[index] != '\0'; index++) {
		if (mode[index] == 'b' && !binary) binary = true;
		else if (mode[index] == '+' && !update) update = true;
		else return false;
	}
	*out_mode = (struct file_mode){
		.readable = initial == 'r' || update,
		.writable = initial != 'r' || update,
		.create   = initial != 'r',
		.truncate = initial == 'w',
		.append   = initial == 'a',
	};
	return true;
}

static void file_capability_drop(cap_id_t* capability) {
	if (capability == NULL || *capability == CAP_ID_INVALID) return;
	(void)cap_drop(*capability);
	*capability = CAP_ID_INVALID;
}

FILE* fopen(const char* restrict path, const char* restrict mode) {
	static const struct init_protocol_query vfs_query = {
		.namespace_path = VFS_NAMESPACE,
		.protocol       = VFS_PROTOCOL_NAME,
		.major          = VFS_PROTOCOL_VERSION_MAJOR,
		.minor          = VFS_PROTOCOL_VERSION_MINOR,
	};
	struct init_service_handle    vfs  = {.capability = CAP_ID_INVALID};
	struct filesystem_node_handle node = {.capability = CAP_ID_INVALID};
	struct filesystem_call_result open_result;
	struct init_call_result       acquire_result;
	struct file_mode              parsed_mode;
	cap_rights_t                  rights = 0u;
	FILE*                         stream = NULL;
	size_t                        path_size;
	bool                          success = false;

	if (path == NULL || !file_mode_parse(mode, &parsed_mode)) return NULL;
	path_size = strlen(path);
	if (!vfs_absolute_path_valid(path, path_size)) return NULL;
	if (parsed_mode.readable) rights |= CAP_READ;
	if (parsed_mode.writable) rights |= CAP_WRITE;

	acquire_result = init_acquire(&vfs_query, VFS_SERVICE_NAME, &vfs);
	if (acquire_result.transport_status != SYSCALL_STATUS_OK || acquire_result.status != INIT_REGISTRY_OK) goto cleanup;
	open_result = vfs_open(vfs.capability, path, path_size, rights, &node);
	if (!call_succeeded(open_result) && parsed_mode.create && open_result.transport_status == SYSCALL_STATUS_OK &&
	    open_result.status == FILESYSTEM_STATUS_NOT_FOUND) {
		open_result = vfs_create_file(vfs.capability, path, path_size, rights, &node);
	}
	if (!call_succeeded(open_result) || node.info.type != FILESYSTEM_NODE_FILE) goto cleanup;
	if (parsed_mode.truncate && !call_succeeded(filesystem_file_resize(node.capability, 0u))) goto cleanup;

	stream = calloc(1u, sizeof(*stream));
	if (stream == NULL) goto cleanup;
	*stream = (FILE){
		.kind       = FILE_KIND_VFS,
		.capability = node.capability,
		.offset     = parsed_mode.append ? node.info.size : 0u,
		.readable   = parsed_mode.readable,
		.writable   = parsed_mode.writable,
	};
	node.capability = CAP_ID_INVALID;
	success         = true;

cleanup:
	file_capability_drop(&node.capability);
	if (vfs.capability != CAP_ID_INVALID) {
		if (cap_drop(vfs.capability) != SYSCALL_STATUS_OK) success = false;
		vfs.capability = CAP_ID_INVALID;
	}
	if (!success) {
		if (stream != NULL) {
			file_capability_drop(&stream->capability);
			free(stream);
			stream = NULL;
		}
	}
	return stream;
}

int fclose(FILE* stream) {
	cap_id_t         capability;
	syscall_status_t status;

	if (stream == NULL) return EOF;
	file_lock(stream);
	if (stream->kind != FILE_KIND_VFS || stream->capability == CAP_ID_INVALID) {
		stream->error = true;
		file_unlock(stream);
		return EOF;
	}
	capability         = stream->capability;
	stream->capability = CAP_ID_INVALID;
	file_unlock(stream);
	status = cap_drop(capability);
	free(stream);
	return status == SYSCALL_STATUS_OK ? 0 : EOF;
}

static size_t file_read(FILE* stream, void* buffer, size_t size) {
	size_t transferred = 0u;

	switch (stream->kind) {
	case FILE_KIND_STANDARD_INPUT:
		if (display_read(buffer, size, &transferred) != SYSCALL_STATUS_OK) stream->error = true;
		break;
	case FILE_KIND_VFS: {
		struct filesystem_call_result result =
			filesystem_file_read(stream->capability, stream->offset, buffer, size, &transferred);
		if (!call_succeeded(result)) stream->error = true;
		break;
	}
	default:
		stream->error = true;
		break;
	}
	return transferred;
}

size_t fread(void* restrict buffer, size_t size, size_t count, FILE* restrict stream) {
	size_t total;
	size_t completed = 0u;

	if (stream == NULL) return 0u;
	if (size == 0u || count == 0u) return 0u;
	file_lock(stream);
	if (buffer == NULL || !stream->readable || count > SIZE_MAX / size) {
		stream->error = true;
		file_unlock(stream);
		return 0u;
	}
	total = size * count;
	if (stream->kind == FILE_KIND_VFS && total > UINT64_MAX - stream->offset) {
		stream->error = true;
		file_unlock(stream);
		return 0u;
	}
	while (completed < total && !stream->error) {
		size_t transferred = file_read(stream, (uint8_t*)buffer + completed, total - completed);

		if (transferred > total - completed) {
			stream->error = true;
			break;
		}
		if (transferred == 0u) {
			if (!stream->error) stream->end_of_file = true;
			break;
		}
		completed += transferred;
		if (stream->kind == FILE_KIND_VFS) stream->offset += transferred;
	}
	file_unlock(stream);
	return completed / size;
}

static size_t file_write(FILE* stream, const void* buffer, size_t size) {
	size_t transferred = 0u;

	switch (stream->kind) {
	case FILE_KIND_STANDARD_OUTPUT:
		if (display_write(buffer, size) == SYSCALL_STATUS_OK) transferred = size;
		else stream->error = true;
		break;
	case FILE_KIND_STANDARD_ERROR:
		if (display_error_write(buffer, size) == SYSCALL_STATUS_OK) transferred = size;
		else stream->error = true;
		break;
	case FILE_KIND_VFS: {
		struct filesystem_call_result result =
			filesystem_file_write(stream->capability, stream->offset, buffer, size, &transferred);
		if (!call_succeeded(result)) stream->error = true;
		break;
	}
	default:
		stream->error = true;
		break;
	}
	return transferred;
}

size_t fwrite(const void* restrict buffer, size_t size, size_t count, FILE* restrict stream) {
	size_t total;
	size_t completed = 0u;

	if (stream == NULL) return 0u;
	if (size == 0u || count == 0u) return 0u;
	file_lock(stream);
	if (buffer == NULL || !stream->writable || count > SIZE_MAX / size) {
		stream->error = true;
		file_unlock(stream);
		return 0u;
	}
	total = size * count;
	if (stream->kind == FILE_KIND_VFS && total > UINT64_MAX - stream->offset) {
		stream->error = true;
		file_unlock(stream);
		return 0u;
	}
	while (completed < total && !stream->error) {
		size_t transferred = file_write(stream, (const uint8_t*)buffer + completed, total - completed);

		if (transferred == 0u || transferred > total - completed) {
			stream->error = true;
			break;
		}
		completed += transferred;
		if (stream->kind == FILE_KIND_VFS) stream->offset += transferred;
	}
	file_unlock(stream);
	return completed / size;
}

int feof(FILE* stream) {
	bool result;

	if (stream == NULL) return 0;
	file_lock(stream);
	result = stream->end_of_file;
	file_unlock(stream);
	return result ? 1 : 0;
}

int ferror(FILE* stream) {
	bool result;

	if (stream == NULL) return 1;
	file_lock(stream);
	result = stream->error;
	file_unlock(stream);
	return result ? 1 : 0;
}

void clearerr(FILE* stream) {
	if (stream == NULL) return;
	file_lock(stream);
	stream->end_of_file = false;
	stream->error       = false;
	file_unlock(stream);
}

int fflush(FILE* stream) {
	if (stream == NULL) return 0;
	file_lock(stream);
	file_unlock(stream);
	return 0;
}
