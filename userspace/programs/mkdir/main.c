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

#define MKDIR_VFS_MAJOR 1u
#define MKDIR_VFS_MINOR 0u

#define MKDIR_PATH_CAPACITY 4096u

struct mkdir_path {
	char   bytes[MKDIR_PATH_CAPACITY];
	size_t size;
	bool   absolute;
};

struct mkdir_context {
	cap_id_t vfs_cap; /* Acquired from registry, owned here. */
	cap_id_t cwd_cap; /* Borrowed from process startup, never drop. */
	bool     parents;
	bool     verbose;
};

static void report_error(const char* operand, const char* message) {
	if (operand != NULL) (void)fprintf(stderr, "mkdir: %s: %s\n", operand, message);
	else (void)fprintf(stderr, "mkdir: %s\n", message);
}

static bool call_ok(struct filesystem_call_result result) {
	return result.transport_status == SYSCALL_STATUS_OK && result.status == FILESYSTEM_STATUS_OK;
}

static const char* filesystem_error(struct filesystem_call_result result) {
	if (result.transport_status == SYSCALL_STATUS_DENIED) return "permission denied";
	if (result.transport_status == SYSCALL_STATUS_BAD_ARGUMENT) return "invalid filesystem request";
	if (result.transport_status != SYSCALL_STATUS_OK) return "filesystem transport failure";
	switch (result.status) {
	case FILESYSTEM_STATUS_NOT_FOUND:
		return "no such file or directory";
	case FILESYSTEM_STATUS_ALREADY_EXISTS:
		return "file exists";
	case FILESYSTEM_STATUS_NOT_DIRECTORY:
		return "not a directory";
	case FILESYSTEM_STATUS_IS_DIRECTORY:
		return "is a directory";
	case FILESYSTEM_STATUS_INVALID_ARGUMENT:
		return "invalid path";
	case FILESYSTEM_STATUS_ACCESS_DENIED:
		return "permission denied";
	case FILESYSTEM_STATUS_READ_ONLY:
		return "read-only filesystem";
	case FILESYSTEM_STATUS_NO_SPACE:
		return "no space left on device";
	case FILESYSTEM_STATUS_CROSS_FILESYSTEM:
		return "cross-filesystem operation";
	case FILESYSTEM_STATUS_BUSY:
		return "resource busy";
	case FILESYSTEM_STATUS_UNSUPPORTED:
		return "operation not supported";
	case FILESYSTEM_STATUS_IO_ERROR:
		return "I/O error";
	default:
		return "filesystem operation failed";
	}
}

static bool pop_component(struct mkdir_path* path) {
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

static const char* normalize_path(const char* operand, struct mkdir_path* path) {
	const size_t length = strlen(operand);
	size_t       offset = 0u;

	if (length == 0u) return "invalid path";
	path->absolute = operand[0] == '/';
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
			if (!pop_component(path)) return "invalid path";
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

static bool acquire_vfs(struct mkdir_context* context) {
	static const struct registry_protocol_query query = {
		.namespace_path = VFS_NAMESPACE,
		.protocol       = VFS_PROTOCOL_NAME,
		.major          = MKDIR_VFS_MAJOR,
		.minor          = MKDIR_VFS_MINOR,
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

static struct filesystem_call_result path_open(struct mkdir_context* context, const struct mkdir_path* path,
                                               size_t prefix_size, struct filesystem_node_handle* node) {
	if (path->absolute) return vfs_open(context->vfs_cap, path->bytes, prefix_size, CAP_READ, node);
	return vfs_directory_open(context->cwd_cap, path->bytes, prefix_size, CAP_READ, node);
}

static struct filesystem_call_result path_create(struct mkdir_context* context, const struct mkdir_path* path,
                                                 size_t prefix_size, struct filesystem_node_handle* node) {
	if (path->absolute) return vfs_create_directory(context->vfs_cap, path->bytes, prefix_size, CAP_READ, node);
	return vfs_directory_create_directory(context->cwd_cap, path->bytes, prefix_size, CAP_READ, node);
}

static bool create_component(struct mkdir_context* context, const struct mkdir_path* path, size_t prefix_size,
                             const char* operand, bool allow_existing) {
	struct filesystem_node_handle node = {.capability = CAP_ID_INVALID};
	struct filesystem_call_result result;
	bool                          created = false;

	if (allow_existing) {
		result = path_open(context, path, prefix_size, &node);
		if (call_ok(result)) {
			bool directory = node.info.type == FILESYSTEM_NODE_DIRECTORY;
			if (node.capability != CAP_ID_INVALID) (void)cap_drop(node.capability);
			if (!directory) report_error(operand, "not a directory");
			return directory;
		}
		if (node.capability != CAP_ID_INVALID) (void)cap_drop(node.capability);
		if (result.transport_status != SYSCALL_STATUS_OK || result.status != FILESYSTEM_STATUS_NOT_FOUND) {
			report_error(operand, filesystem_error(result));
			return false;
		}
	}

	node.capability = CAP_ID_INVALID;
	result          = path_create(context, path, prefix_size, &node);
	if (call_ok(result)) created = true;
	else if (allow_existing && result.transport_status == SYSCALL_STATUS_OK &&
	         result.status == FILESYSTEM_STATUS_ALREADY_EXISTS) {
		/* Another process may have created this component after our lookup. */
		if (node.capability != CAP_ID_INVALID) (void)cap_drop(node.capability);
		node.capability = CAP_ID_INVALID;
		result          = path_open(context, path, prefix_size, &node);
		if (call_ok(result) && node.info.type == FILESYSTEM_NODE_DIRECTORY) {
			if (node.capability != CAP_ID_INVALID) (void)cap_drop(node.capability);
			return true;
		}
		if (call_ok(result)) report_error(operand, "not a directory");
		else report_error(operand, filesystem_error(result));
		if (node.capability != CAP_ID_INVALID) (void)cap_drop(node.capability);
		return false;
	}
	else report_error(operand, filesystem_error(result));

	if (node.capability != CAP_ID_INVALID) (void)cap_drop(node.capability);
	if (!created) return false;
	if (context->verbose && fprintf(stdout, "mkdir: created directory '%.*s'\n", (int)prefix_size, path->bytes) < 0) {
		report_error(operand, "cannot write standard output");
		return false;
	}
	return true;
}

static bool create_operand(struct mkdir_context* context, const char* operand) {
	struct mkdir_path path;
	const char*       error = normalize_path(operand, &path);

	if (error != NULL) {
		report_error(operand, error);
		return false;
	}
	if (path.absolute) {
		if (!acquire_vfs(context)) {
			report_error(operand, "VFS service unavailable");
			return false;
		}
	}
	else if (context->cwd_cap == CAP_ID_INVALID) {
		report_error(operand, "working directory unavailable");
		return false;
	}

	/* '.' means the already-open cwd, '/' the already-existing VFS root. */
	if (path.size == 0u || (path.absolute && path.size == 1u)) {
		if (!context->parents) report_error(operand, "file exists");
		return context->parents;
	}
	if (!context->parents) return create_component(context, &path, path.size, operand, false);

	/* -p: ensure each prefix is a directory, creating only missing components. */
	for (size_t offset = path.absolute ? 1u : 0u; offset < path.size;) {
		size_t prefix_size = offset;
		while (prefix_size < path.size && path.bytes[prefix_size] != '/') prefix_size++;
		if (!create_component(context, &path, prefix_size, operand, true)) return false;
		offset = prefix_size + 1u;
	}
	return true;
}

static void print_usage(void) {
	(void)printf("Usage: mkdir [-p] [-v] [--] DIRECTORY...\n"
	             "  -p, --parents    create missing parent directories; accept existing directories\n"
	             "  -v, --verbose    print each directory actually created\n"
	             "      --help       show this help\n");
}

int main(int argc, char** argv, size_t capc, const cap_id_t* capv) {
	struct mkdir_context context = {
		.vfs_cap = CAP_ID_INVALID,
		.cwd_cap = filesystem_get_working_directory(),
	};
	bool after_separator = false;
	bool success         = true;
	bool has_operand     = false;

	if (argc < 0 || (argc != 0 && argv == NULL) || capc != 0u || capv != NULL) return 1;
	for (int i = 0; i < argc; i++) {
		const char* argument = argv[i];
		if (!after_separator && argument[0] == '-' && argument[1] != '\0') {
			if (strcmp(argument, "--") == 0) {
				after_separator = true;
				continue;
			}
			if (strcmp(argument, "--help") == 0) {
				print_usage();
				return 0;
			}
			if (strcmp(argument, "--parents") == 0) context.parents = true;
			else if (strcmp(argument, "--verbose") == 0) context.verbose = true;
			else if (argument[1] == '-') {
				report_error(argument, "unrecognized option");
				return 1;
			}
			else {
				for (const char* letter = argument + 1; *letter != '\0'; letter++) {
					if (*letter == 'p') context.parents = true;
					else if (*letter == 'v') context.verbose = true;
					else {
						report_error(argument, "unrecognized option");
						return 1;
					}
				}
			}
			continue;
		}
		has_operand = true;
	}
	if (!has_operand) {
		report_error(NULL, "missing directory operand");
		return 1;
	}

	after_separator = false;
	for (int i = 0; i < argc; i++) {
		const char* argument = argv[i];
		if (!after_separator && argument[0] == '-' && argument[1] != '\0') {
			if (strcmp(argument, "--") == 0) after_separator = true;
			continue;
		}
		if (!create_operand(&context, argument)) success = false;
	}
	if (context.vfs_cap != CAP_ID_INVALID) (void)cap_drop(context.vfs_cap);
	return success ? 0 : 1;
}
