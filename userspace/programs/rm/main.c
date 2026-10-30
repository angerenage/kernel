#include <base/cap.h>
#include <libc/stdlib.h>
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

#define RM_VFS_MAJOR 1u
#define RM_VFS_MINOR 0u

enum {
	RM_PATH_CAPACITY          = 4096u,
	RM_INITIAL_ENTRY_CAPACITY = (CAP_MAX_RESPONSE_SIZE - sizeof(struct filesystem_directory_enumerate_response)) /
		sizeof(struct filesystem_directory_entry),
};

struct rm_path {
	char   bytes[RM_PATH_CAPACITY];
	size_t size;
	bool   absolute;
};

struct rm_context {
	cap_id_t vfs_cap;
	cap_id_t cwd_cap;
	bool     recursive;
	bool     force;
	bool     verbose;
};

static void report_error(const char* operand, const char* message) {
	if (operand != NULL) (void)fprintf(stderr, "rm: %s: %s\n", operand, message);
	else (void)fprintf(stderr, "rm: %s\n", message);
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
	case FILESYSTEM_STATUS_DIRECTORY_NOT_EMPTY:
		return "directory not empty";
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

static bool pop_component(struct rm_path* path) {
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

static const char* normalize_path(const char* operand, struct rm_path* path) {
	const size_t length = strlen(operand);
	size_t       offset = 0u;

	if (length == 0u) return "invalid path";
	path->absolute = operand[0u] == '/';
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

static bool final_component_is_parent(const struct rm_path* path) {
	if (path->absolute || path->size < 2u) return false;
	size_t start = path->size;
	while (start != 0u && path->bytes[start - 1u] != '/') start--;
	return path->size - start == 2u && path->bytes[start] == '.' && path->bytes[start + 1u] == '.';
}

static bool acquire_vfs(struct rm_context* context) {
	static const struct registry_protocol_query query = {
		.namespace_path = VFS_NAMESPACE,
		.protocol       = VFS_PROTOCOL_NAME,
		.major          = RM_VFS_MAJOR,
		.minor          = RM_VFS_MINOR,
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

static struct filesystem_call_result path_open(struct rm_context* context, const struct rm_path* path,
                                               cap_rights_t rights, struct filesystem_node_handle* node) {
	if (path->absolute) return vfs_open(context->vfs_cap, path->bytes, path->size, rights, node);
	return vfs_directory_open(context->cwd_cap, path->bytes, path->size, rights, node);
}

static struct filesystem_call_result path_remove(struct rm_context* context, const struct rm_path* path) {
	if (path->absolute) return vfs_remove(context->vfs_cap, path->bytes, path->size);
	return vfs_directory_remove(context->cwd_cap, path->bytes, path->size);
}

static const char* enumerate_directory(cap_id_t directory_cap, struct filesystem_directory_entry** out_entries,
                                       size_t* out_count) {
	struct filesystem_directory_entry* entries  = NULL;
	size_t                             count    = 0u;
	size_t                             capacity = 0u;
	uint64_t                           total    = 0u;

	for (;;) {
		if (count == capacity) {
			size_t next = capacity == 0u ? RM_INITIAL_ENTRY_CAPACITY : capacity * 2u;
			if (total > next && total <= SIZE_MAX / sizeof(*entries)) next = (size_t)total;
			struct filesystem_directory_entry* grown =
				next > SIZE_MAX / sizeof(*entries) ? NULL : realloc(entries, next * sizeof(*entries));
			if (grown == NULL) {
				free(entries);
				return "out of memory";
			}
			entries  = grown;
			capacity = next;
		}
		size_t                        returned;
		struct filesystem_call_result result =
			vfs_directory_enumerate(directory_cap, count, entries + count, capacity - count, &returned, &total);
		if (!call_ok(result)) {
			free(entries);
			return filesystem_error(result);
		}
		count += returned;
		if (returned == 0u || count >= total) break;
	}
	*out_entries = entries;
	*out_count   = count;
	return NULL;
}

static bool append_child_path(const struct rm_path* parent, const struct filesystem_directory_entry* entry,
                              struct rm_path* child) {
	*child                = *parent;
	size_t separator_size = child->size > (child->absolute ? 1u : 0u) ? 1u : 0u;
	if (entry->name_size == 0u || entry->name_size > FILESYSTEM_NAME_MAX ||
	    separator_size + entry->name_size > sizeof(child->bytes) - child->size)
		return false;
	if (separator_size != 0u) child->bytes[child->size++] = '/';
	memcpy(child->bytes + child->size, entry->name, entry->name_size);
	child->size += entry->name_size;
	return true;
}

static bool remove_child(struct rm_context* context, cap_id_t parent_cap,
                         const struct filesystem_directory_entry* entry, const struct rm_path* parent_path,
                         const char* operand, bool* deferred_removal) {
	struct rm_path child_path;
	if (!append_child_path(parent_path, entry, &child_path)) {
		report_error(operand, "path too long");
		return false;
	}
	if ((entry->info.flags & FILESYSTEM_NODE_MOUNT_POINT) != 0u) {
		report_error(operand, "resource busy");
		return false;
	}
	if (entry->info.type == FILESYSTEM_NODE_DIRECTORY) {
		struct filesystem_node_handle child = {.capability = CAP_ID_INVALID};
		struct filesystem_call_result result =
			vfs_directory_open(parent_cap, entry->name, entry->name_size, CAP_READ | CAP_WRITE, &child);
		if (!call_ok(result)) {
			if (child.capability != CAP_ID_INVALID) (void)cap_drop(child.capability);
			if (context->force && result.transport_status == SYSCALL_STATUS_OK &&
			    result.status == FILESYSTEM_STATUS_NOT_FOUND)
				return true;
			report_error(operand, filesystem_error(result));
			return false;
		}
		struct filesystem_directory_entry* entries;
		size_t                             count;
		const char*                        error = enumerate_directory(child.capability, &entries, &count);
		if (error != NULL) {
			(void)cap_drop(child.capability);
			report_error(operand, error);
			return false;
		}
		bool children_removed = true;
		for (size_t index = 0u; index < count; index++)
			if (!remove_child(context, child.capability, &entries[index], &child_path, operand, deferred_removal))
				children_removed = false;
		free(entries);
		if (cap_drop(child.capability) != SYSCALL_STATUS_OK) {
			report_error(operand, "cannot close directory");
			return false;
		}
		if (!children_removed) return false;
	}

	struct filesystem_call_result result = vfs_directory_remove_deferred(parent_cap, entry->name, entry->name_size);
	if (result.transport_status == SYSCALL_STATUS_OK && result.status == FILESYSTEM_STATUS_UNSUPPORTED)
		result = vfs_directory_remove(parent_cap, entry->name, entry->name_size);
	else if (call_ok(result)) *deferred_removal = true;
	if (!call_ok(result)) {
		if (context->force && result.transport_status == SYSCALL_STATUS_OK &&
		    result.status == FILESYSTEM_STATUS_NOT_FOUND)
			return true;
		report_error(operand, filesystem_error(result));
		return false;
	}
	if (context->verbose && fprintf(stdout, "removed '%.*s'\n", (int)child_path.size, child_path.bytes) < 0) {
		report_error(operand, "cannot write standard output");
		return false;
	}
	return true;
}

static bool remove_path(struct rm_context* context, const struct rm_path* path, const char* operand) {
	struct filesystem_node_handle node   = {.capability = CAP_ID_INVALID};
	cap_rights_t                  rights = context->recursive ? CAP_READ | CAP_WRITE : CAP_READ;
	struct filesystem_call_result result = path_open(context, path, rights, &node);
	if (!call_ok(result)) {
		if (node.capability != CAP_ID_INVALID) (void)cap_drop(node.capability);
		if (context->force && result.transport_status == SYSCALL_STATUS_OK &&
		    result.status == FILESYSTEM_STATUS_NOT_FOUND)
			return true;
		report_error(operand, filesystem_error(result));
		return false;
	}
	bool directory = node.info.type == FILESYSTEM_NODE_DIRECTORY;
	if ((node.info.flags & FILESYSTEM_NODE_MOUNT_POINT) != 0u) {
		(void)cap_drop(node.capability);
		report_error(operand, "resource busy");
		return false;
	}
	if (directory && context->recursive) {
		struct filesystem_directory_entry* entries;
		size_t                             count;
		const char*                        error = enumerate_directory(node.capability, &entries, &count);
		if (error != NULL) {
			(void)cap_drop(node.capability);
			report_error(operand, error);
			return false;
		}
		bool children_removed = true;
		bool deferred_removal = false;
		for (size_t index = 0u; index < count; index++) {
			if (!remove_child(context, node.capability, &entries[index], path, operand, &deferred_removal))
				children_removed = false;
		}
		free(entries);
		if (deferred_removal) {
			result = vfs_directory_sync(node.capability);
			if (!call_ok(result)) {
				report_error(operand, filesystem_error(result));
				children_removed = false;
			}
		}
		if (cap_drop(node.capability) != SYSCALL_STATUS_OK) {
			report_error(operand, "cannot close directory");
			return false;
		}
		node.capability = CAP_ID_INVALID;
		if (!children_removed) return false;
	}
	if (node.capability != CAP_ID_INVALID) {
		if (cap_drop(node.capability) != SYSCALL_STATUS_OK) {
			report_error(operand, "cannot close filesystem node");
			return false;
		}
		node.capability = CAP_ID_INVALID;
	}
	result = path_remove(context, path);
	if (!call_ok(result)) {
		if (context->force && result.transport_status == SYSCALL_STATUS_OK &&
		    result.status == FILESYSTEM_STATUS_NOT_FOUND)
			return true;
		report_error(operand, filesystem_error(result));
		return false;
	}
	if (context->verbose && fprintf(stdout, "removed '%.*s'\n", (int)path->size, path->bytes) < 0) {
		report_error(operand, "cannot write standard output");
		return false;
	}
	return true;
}

static bool remove_operand(struct rm_context* context, const char* operand) {
	struct rm_path path;
	const char*    error = normalize_path(operand, &path);

	if (error != NULL) {
		report_error(operand, error);
		return false;
	}
	if (path.size == 0u || (path.absolute && path.size == 1u)) {
		report_error(operand, "refusing to remove the working directory or filesystem root");
		return false;
	}
	if (final_component_is_parent(&path)) {
		report_error(operand, "refusing to remove '..'");
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
	return remove_path(context, &path, operand);
}

static void print_usage(void) {
	(void)printf("Usage: rm [-fRrv] [--] FILE...\n"
	             "Remove files and empty directories.\n"
	             "  -f, --force       ignore nonexistent operands\n"
	             "  -r, -R, --recursive  remove directory contents recursively\n"
	             "  -v, --verbose     print each removed path\n"
	             "      --help        show this help\n");
}

static bool parse_option(struct rm_context* context, const char* argument) {
	if (strcmp(argument, "--force") == 0) context->force = true;
	else if (strcmp(argument, "--recursive") == 0) context->recursive = true;
	else if (strcmp(argument, "--verbose") == 0) context->verbose = true;
	else if (argument[1u] == '-') return false;
	else {
		for (const char* letter = argument + 1u; *letter != '\0'; letter++) {
			if (*letter == 'f') context->force = true;
			else if (*letter == 'r' || *letter == 'R') context->recursive = true;
			else if (*letter == 'v') context->verbose = true;
			else return false;
		}
	}
	return true;
}

int main(int argc, char** argv, size_t capc, const cap_id_t* capv) {
	struct rm_context context = {
		.vfs_cap = CAP_ID_INVALID,
		.cwd_cap = filesystem_get_working_directory(),
	};
	bool after_separator = false;
	bool has_operand     = false;
	bool success         = true;

	if (argc < 0 || (argc != 0 && argv == NULL) || capc != 0u || capv != NULL) return 1;
	for (int index = 0; index < argc; index++) {
		const char* argument = argv[index];
		if (!after_separator && argument[0u] == '-' && argument[1u] != '\0') {
			if (strcmp(argument, "--") == 0) {
				after_separator = true;
				continue;
			}
			if (strcmp(argument, "--help") == 0) {
				print_usage();
				return 0;
			}
			if (!parse_option(&context, argument)) {
				report_error(argument, "unrecognized option");
				return 1;
			}
			continue;
		}
		has_operand = true;
	}
	if (!has_operand) {
		if (context.force) return 0;
		report_error(NULL, "missing operand");
		return 1;
	}

	after_separator = false;
	for (int index = 0; index < argc; index++) {
		const char* argument = argv[index];
		if (!after_separator && argument[0u] == '-' && argument[1u] != '\0') {
			if (strcmp(argument, "--") == 0) after_separator = true;
			continue;
		}
		if (!remove_operand(&context, argument)) success = false;
	}
	if (context.vfs_cap != CAP_ID_INVALID) (void)cap_drop(context.vfs_cap);
	return success ? 0 : 1;
}
