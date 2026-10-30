#include <base/cap.h>
#include <libc/string.h>
#include <protocol/filesystem.h>
#include <protocol/vfs.h>
#include <runtime/filesystem.h>
#include <runtime/registry.h>
#include <runtime/vfs.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <system/capability.h>
#include <system/display.h>

#define CAT_VFS_MAJOR 1u
#define CAT_VFS_MINOR 0u

enum {
	CAT_PATH_CAPACITY = 4096u,
	CAT_BUFFER_SIZE   = 4096u,
};

struct cat_path {
	char   bytes[CAT_PATH_CAPACITY];
	size_t size;
	bool   absolute;
};

struct cat_output {
	uint8_t  bytes[CAT_BUFFER_SIZE];
	size_t   used;
	uint64_t next_line;
	bool     at_line_start;
	bool     number_lines;
	bool     number_nonblank;
	bool     failed;
};

struct cat_context {
	cap_id_t          vfs_cap;
	cap_id_t          cwd_cap;
	struct cat_output output;
};

static void cat_error(const char* operand, const char* message) {
	if (operand != NULL) (void)fprintf(stderr, "cat: %s: %s\n", operand, message);
	else (void)fprintf(stderr, "cat: %s\n", message);
}

static bool call_succeeded(struct filesystem_call_result result) {
	return result.transport_status == SYSCALL_STATUS_OK && result.status == FILESYSTEM_STATUS_OK;
}

static const char* filesystem_error(struct filesystem_call_result result) {
	if (result.transport_status == SYSCALL_STATUS_DENIED) return "permission denied";
	if (result.transport_status != SYSCALL_STATUS_OK) return "filesystem transport failure";
	switch (result.status) {
	case FILESYSTEM_STATUS_NOT_FOUND:
		return "no such file or directory";
	case FILESYSTEM_STATUS_ACCESS_DENIED:
		return "permission denied";
	case FILESYSTEM_STATUS_IS_DIRECTORY:
		return "is a directory";
	case FILESYSTEM_STATUS_NOT_DIRECTORY:
		return "not a directory";
	case FILESYSTEM_STATUS_INVALID_ARGUMENT:
		return "invalid path";
	case FILESYSTEM_STATUS_IO_ERROR:
		return "I/O error";
	case FILESYSTEM_STATUS_UNSUPPORTED:
		return "operation not supported";
	default:
		return "filesystem operation failed";
	}
}

static bool path_pop(struct cat_path* path) {
	const size_t root = path->absolute ? 1u : 0u;

	if (path->size == root) {
		if (path->absolute) return true;
		memcpy(path->bytes, "..", 2u);
		path->size = 2u;
		return true;
	}
	size_t component = path->size;
	while (component > root && path->bytes[component - 1u] != '/') component--;
	if (!path->absolute && path->size - component == 2u && path->bytes[component] == '.' &&
	    path->bytes[component + 1u] == '.') {
		if (sizeof(path->bytes) - path->size < 3u) return false;
		memcpy(path->bytes + path->size, "/..", 3u);
		path->size += 3u;
		return true;
	}
	while (path->size > root && path->bytes[path->size - 1u] != '/') path->size--;
	if (path->size > root) path->size--;
	return true;
}

static const char* normalize_path(const char* operand, struct cat_path* path) {
	const size_t length = strlen(operand);
	size_t       offset = 0u;

	path->absolute = length != 0u && operand[0] == '/';
	path->size     = 0u;
	if (path->absolute) path->bytes[path->size++] = '/';
	while (offset < length) {
		size_t start;
		size_t component_size;
		size_t separator_size;

		while (offset < length && operand[offset] == '/') offset++;
		start = offset;
		while (offset < length && operand[offset] != '/') offset++;
		component_size = offset - start;
		if (component_size == 0u || (component_size == 1u && operand[start] == '.')) continue;
		if (component_size == 2u && operand[start] == '.' && operand[start + 1u] == '.') {
			if (!path_pop(path)) return "invalid path";
			continue;
		}
		separator_size = path->size > (path->absolute ? 1u : 0u) ? 1u : 0u;
		if (component_size > FILESYSTEM_NAME_MAX || separator_size + component_size > sizeof(path->bytes) - path->size)
			return "invalid path";
		if (separator_size != 0u) path->bytes[path->size++] = '/';
		memcpy(path->bytes + path->size, operand + start, component_size);
		path->size += component_size;
	}
	return NULL;
}

static bool acquire_vfs(struct cat_context* context) {
	static const struct registry_protocol_query query = {
		.namespace_path = VFS_NAMESPACE,
		.protocol       = VFS_PROTOCOL_NAME,
		.major          = CAT_VFS_MAJOR,
		.minor          = CAT_VFS_MINOR,
	};
	struct registry_service_handle service = {.capability = CAP_ID_INVALID};
	struct registry_call_result    result;

	if (context->vfs_cap != CAP_ID_INVALID) return true;
	result = registry_acquire(&query, VFS_SERVICE_NAME, &service);
	if (result.transport_status != SYSCALL_STATUS_OK || result.status != REGISTRY_STATUS_OK ||
	    service.capability == CAP_ID_INVALID) {
		if (service.capability != CAP_ID_INVALID) (void)cap_drop(service.capability);
		return false;
	}
	context->vfs_cap = service.capability;
	return true;
}

static bool output_flush(struct cat_output* output) {
	if (output->failed) return false;
	if (output->used == 0u) return true;
	if (display_write((const char*)output->bytes, output->used) != SYSCALL_STATUS_OK) {
		output->failed = true;
		return false;
	}
	output->used = 0u;
	return true;
}

static bool output_append(struct cat_output* output, const void* bytes, size_t size) {
	const uint8_t* input = bytes;

	while (size != 0u) {
		size_t available;
		size_t amount;

		if (output->used == sizeof(output->bytes) && !output_flush(output)) return false;
		available = sizeof(output->bytes) - output->used;
		amount    = size < available ? size : available;
		memcpy(output->bytes + output->used, input, amount);
		output->used += amount;
		input += amount;
		size -= amount;
	}
	return true;
}

static bool output_numbered(struct cat_output* output, const uint8_t* input, size_t size) {
	for (size_t i = 0u; i < size; i++) {
		uint8_t ch = input[i];

		if (output->at_line_start && (!output->number_nonblank || ch != '\n')) {
			char prefix[32];
			int  length = snprintf(prefix, sizeof(prefix), "%6llu\t", (unsigned long long)output->next_line);

			if (length < 0 || (size_t)length >= sizeof(prefix) || !output_append(output, prefix, (size_t)length))
				return false;
			if (output->next_line != UINT64_MAX) output->next_line++;
		}
		if (!output_append(output, &ch, 1u)) return false;
		output->at_line_start = ch == '\n';
	}
	return true;
}

static bool output_bytes(struct cat_output* output, const uint8_t* input, size_t size) {
	if (!output->number_lines && !output->number_nonblank) return output_append(output, input, size);
	return output_numbered(output, input, size);
}

static bool cat_standard_input(struct cat_context* context) {
	uint8_t buffer[CAT_BUFFER_SIZE];

	for (;;) {
		size_t read_size = 0u;
		if (display_read(buffer, sizeof(buffer), &read_size) != SYSCALL_STATUS_OK) {
			cat_error("-", "standard input read failed");
			return false;
		}
		if (read_size == 0u) return true;
		if (!output_bytes(&context->output, buffer, read_size)) return false;
		if (!output_flush(&context->output)) return false;
	}
}

static bool cat_file(struct cat_context* context, const char* operand) {
	struct filesystem_node_handle node = {.capability = CAP_ID_INVALID};
	struct filesystem_call_result result;
	struct cat_path               path;
	uint8_t                       buffer[CAT_BUFFER_SIZE];
	uint64_t                      offset = 0u;
	const char*                   error;
	bool                          success = false;

	error = normalize_path(operand, &path);
	if (error != NULL) {
		cat_error(operand, error);
		return false;
	}
	if (!path.absolute && path.size == 0u) {
		cat_error(operand, "is a directory");
		return false;
	}
	if (path.absolute) {
		if (!acquire_vfs(context)) {
			cat_error(operand, "VFS service unavailable");
			return false;
		}
		result = vfs_open(context->vfs_cap, path.bytes, path.size, CAP_READ, &node);
	}
	else {
		if (context->cwd_cap == CAP_ID_INVALID) {
			cat_error(operand, "working directory unavailable");
			return false;
		}
		result = vfs_directory_open(context->cwd_cap, path.bytes, path.size, CAP_READ, &node);
	}
	if (!call_succeeded(result)) {
		cat_error(operand, filesystem_error(result));
		goto cleanup;
	}
	if (node.capability == CAP_ID_INVALID) {
		cat_error(operand, "invalid file capability");
		goto cleanup;
	}
	if (node.info.type != FILESYSTEM_NODE_FILE) {
		cat_error(operand, "is a directory");
		goto cleanup;
	}
	for (;;) {
		size_t transferred = 0u;

		result = filesystem_file_read(node.capability, offset, buffer, sizeof(buffer), &transferred);
		if (!call_succeeded(result)) {
			cat_error(operand, filesystem_error(result));
			goto cleanup;
		}
		if (transferred == 0u) break;
		if (transferred > sizeof(buffer) || transferred > UINT64_MAX - offset) {
			cat_error(operand, "invalid file read response");
			goto cleanup;
		}
		if (!output_bytes(&context->output, buffer, transferred)) goto cleanup;
		offset += transferred;
	}
	success = true;

cleanup:
	if (node.capability != CAP_ID_INVALID) (void)cap_drop(node.capability);
	return success;
}

static bool is_operand(const char* argument, bool after_separator) {
	return after_separator || argument[0] != '-' || argument[1] == '\0';
}

int main(int argc, char** argv, size_t capc, const cap_id_t* capv) {
	struct cat_context context = {
		.vfs_cap = CAP_ID_INVALID,
		.cwd_cap = filesystem_get_working_directory(),
		.output  = {.next_line = 1u, .at_line_start = true},
	};
	bool   after_separator = false;
	bool   success         = true;
	size_t operands        = 0u;

	if (argc < 0 || (argc != 0 && argv == NULL) || capc != 0u || capv != NULL) return 1;
	for (int i = 0; i < argc; i++) {
		if (is_operand(argv[i], after_separator)) {
			operands++;
			continue;
		}
		if (strcmp(argv[i], "--") == 0) {
			after_separator = true;
			continue;
		}
		if (strcmp(argv[i], "--help") == 0) {
			static const char usage[] = "Usage: cat [-nbu] [--] [FILE ...]\n"
										"Concatenate files to standard output; '-' or no files reads stdin.\n"
										"  -n  number all output lines\n"
										"  -b  number nonempty output lines (overrides -n)\n"
										"  -u  accepted for compatibility (no effect)\n";
			return display_write(usage, sizeof(usage) - 1u) == SYSCALL_STATUS_OK ? 0 : 1;
		}
		if (argv[i][1] == '-') {
			cat_error(argv[i], "unrecognized option");
			return 1;
		}
		for (const char* option = argv[i] + 1; *option != '\0'; option++) {
			switch (*option) {
			case 'n':
				context.output.number_lines = true;
				break;
			case 'b':
				context.output.number_nonblank = true;
				break;
			case 'u':
				break;
			default:
				(void)fprintf(stderr, "cat: invalid option -- '%c'\n", *option);
				return 1;
			}
		}
	}
	if (operands == 0u) success = cat_standard_input(&context);
	else {
		after_separator = false;
		for (int i = 0; i < argc; i++) {
			if (!is_operand(argv[i], after_separator)) {
				if (strcmp(argv[i], "--") == 0) after_separator = true;
				continue;
			}
			if (strcmp(argv[i], "-") == 0) {
				if (!cat_standard_input(&context)) success = false;
			}
			else if (!cat_file(&context, argv[i])) success = false;
			if (context.output.failed) break;
		}
	}
	if (!output_flush(&context.output)) success = false;
	if (context.output.failed) cat_error(NULL, "standard output write failed");
	if (context.vfs_cap != CAP_ID_INVALID) (void)cap_drop(context.vfs_cap);
	return success ? 0 : 1;
}
