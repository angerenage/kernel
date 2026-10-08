#include "server.h"

#include <base/cap.h>
#include <base/process.h>
#include <protocol/vfs.h>
#include <runtime/filesystem.h>
#include <runtime/init.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <system/capability.h>
#include <system/channel.h>
#include <system/process.h>
#include <system/signal.h>

#define VFS_SERVICE_OBJECT_ID 1u
#define VFS_SERVICE_CLIENT_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_READ | CAP_WRITE | CAP_MANAGE))

struct vfs_mount {
	uint64_t                    id;
	char*                       path;
	size_t                      path_size;
	cap_id_t                    root_cap;
	struct filesystem_node_info root_info;
	bool                        read_only;
	struct vfs_mount*           next;
};

struct vfs_directory {
	char*                 path;
	size_t                path_size;
	cap_id_t              raw_cap;
	uint64_t              mount_id;
	bool                  read_only;
	bool                  attached;
	struct vfs_directory* next;
};

struct rename_update {
	struct vfs_directory* directory;
	char*                 path;
	size_t                path_size;
	struct rename_update* next;
};

static channel_id_t          vfs_endpoint    = CHANNEL_ID_INVALID;
static cap_id_t              vfs_activity    = CAP_ID_INVALID;
static cap_id_t              vfs_service_cap = CAP_ID_INVALID;
static process_id_t          vfs_pid         = PROCESS_PID_INVALID;
static struct vfs_mount*     mounts;
static struct vfs_directory* directories;
static uint64_t              next_mount_id = 1u;
static uint8_t               request_buffer[CAP_MAX_REQUEST_SIZE];

static bool reply_request(cap_call_id_t call_id, const void* response, size_t response_size, syscall_status_t status) {
	return channel_reply(call_id, response, response_size, status) == SYSCALL_STATUS_OK;
}

static bool reply_transport(const struct cap_request* call, syscall_status_t status) {
	return reply_request(call->call_id, NULL, 0u, status);
}

static bool reply_domain(const struct cap_request* call, enum filesystem_status status) {
	const struct filesystem_response_header response = {
		.status   = status,
		.reserved = 0u,
	};
	return reply_request(call->call_id, &response, sizeof(response), SYSCALL_STATUS_OK);
}

static bool reply_call_failure(const struct cap_request* call, struct filesystem_call_result result) {
	if (result.transport_status != SYSCALL_STATUS_OK) return reply_transport(call, result.transport_status);
	return reply_domain(call, result.status);
}

static bool copy_request(const struct cap_request* call, const void* data, void* out, size_t size) {
	if (call == NULL || data == NULL || out == NULL || call->request_size < size) return false;
	memcpy(out, data, size);
	return true;
}

static bool request_has_rights(const struct cap_request* call, cap_rights_t rights) {
	return (call->rights & rights) == rights;
}

static bool node_rights_valid(cap_rights_t rights) {
	return (rights & ~FILESYSTEM_NODE_REQUEST_RIGHTS) == 0u;
}

static bool absolute_path_valid(const void* path, size_t path_size) {
	const uint8_t* bytes = path;

	if (path == NULL || path_size == 0u || bytes[0] != '/') return false;
	return path_size == 1u || filesystem_relative_path_valid(bytes + 1u, path_size - 1u);
}

static char* path_copy(const void* path, size_t path_size) {
	char* copy = malloc(path_size);
	if (copy != NULL) memcpy(copy, path, path_size);
	return copy;
}

static bool path_equal(const char* left, size_t left_size, const char* right, size_t right_size) {
	return left_size == right_size && memcmp(left, right, left_size) == 0;
}

static bool path_contains(const char* parent, size_t parent_size, const char* path, size_t path_size) {
	if (parent_size == 1u && parent[0] == '/') return path_size != 0u && path[0] == '/';
	if (path_size < parent_size || memcmp(parent, path, parent_size) != 0) return false;
	return path_size == parent_size || path[parent_size] == '/';
}

static uint64_t directory_object_id(const struct vfs_directory* directory) {
	return (uint64_t)(uintptr_t)directory;
}

static struct vfs_directory* find_directory(uint64_t object_id) {
	for (struct vfs_directory* directory = directories; directory != NULL; directory = directory->next) {
		if (directory_object_id(directory) == object_id) return directory;
	}
	return NULL;
}

static void unlink_directory(struct vfs_directory* target) {
	struct vfs_directory** cursor = &directories;
	while (*cursor != NULL) {
		if (*cursor == target) {
			*cursor      = target->next;
			target->next = NULL;
			return;
		}
		cursor = &(*cursor)->next;
	}
}

static void release_directory(struct vfs_directory* directory) {
	if (directory == NULL) return;
	if (directory->raw_cap != CAP_ID_INVALID) (void)cap_drop(directory->raw_cap);
	free(directory->path);
	free(directory);
}

static struct vfs_mount* find_exact_mount(const char* path, size_t path_size) {
	for (struct vfs_mount* mount = mounts; mount != NULL; mount = mount->next) {
		if (path_equal(mount->path, mount->path_size, path, path_size)) return mount;
	}
	return NULL;
}

static struct vfs_mount* find_mount(const char* path, size_t path_size) {
	for (struct vfs_mount* mount = mounts; mount != NULL; mount = mount->next) {
		if (path_contains(mount->path, mount->path_size, path, path_size)) return mount;
	}
	return NULL;
}

static bool mount_at_or_below(const char* path, size_t path_size) {
	for (const struct vfs_mount* mount = mounts; mount != NULL; mount = mount->next) {
		if (path_contains(path, path_size, mount->path, mount->path_size)) return true;
	}
	return false;
}

static bool mount_has_children(const struct vfs_mount* target) {
	for (const struct vfs_mount* mount = mounts; mount != NULL; mount = mount->next) {
		if (mount != target && mount->path_size > target->path_size &&
		    path_contains(target->path, target->path_size, mount->path, mount->path_size))
			return true;
	}
	return false;
}

static void insert_mount(struct vfs_mount* mount) {
	struct vfs_mount** cursor = &mounts;
	while (*cursor != NULL && (*cursor)->path_size >= mount->path_size) cursor = &(*cursor)->next;
	mount->next = *cursor;
	*cursor     = mount;
}

static void unlink_mount(struct vfs_mount* target) {
	struct vfs_mount** cursor = &mounts;
	while (*cursor != NULL) {
		if (*cursor == target) {
			*cursor      = target->next;
			target->next = NULL;
			return;
		}
		cursor = &(*cursor)->next;
	}
}

static void relative_path(const struct vfs_mount* mount, const char* path, size_t path_size, const void** out_path,
                          size_t* out_size) {
	if (path_size == mount->path_size) {
		*out_path = NULL;
		*out_size = 0u;
		return;
	}
	if (mount->path_size == 1u) {
		*out_path = path + 1u;
		*out_size = path_size - 1u;
		return;
	}
	*out_path = path + mount->path_size + 1u;
	*out_size = path_size - mount->path_size - 1u;
}

static void apply_mount_flags(const char* path, size_t path_size, bool read_only, struct filesystem_node_info* info) {
	const struct vfs_mount* exact = find_exact_mount(path, path_size);
	if (read_only || (exact != NULL && exact->read_only)) info->flags |= FILESYSTEM_NODE_READ_ONLY;
	if (exact != NULL) info->flags |= FILESYSTEM_NODE_MOUNT_POINT;
}

static bool mount_is_direct_child(const struct vfs_mount* mount, const struct vfs_directory* directory,
                                  const struct filesystem_directory_entry* entry) {
	const char* name;
	size_t      name_size;

	if (directory->path_size == 1u) {
		if (mount->path_size <= 1u) return false;
		name      = mount->path + 1u;
		name_size = mount->path_size - 1u;
	}
	else {
		if (mount->path_size <= directory->path_size + 1u ||
		    memcmp(mount->path, directory->path, directory->path_size) != 0 || mount->path[directory->path_size] != '/')
			return false;
		name      = mount->path + directory->path_size + 1u;
		name_size = mount->path_size - directory->path_size - 1u;
	}
	return memchr(name, '/', name_size) == NULL && entry->name_size == name_size &&
	       memcmp(entry->name, name, name_size) == 0;
}

static void patch_enumeration(struct vfs_directory* directory, struct filesystem_directory_entry* entries,
                              size_t count) {
	if (!directory->attached) return;
	for (struct vfs_mount* mount = mounts; mount != NULL; mount = mount->next) {
		for (size_t i = 0u; i < count; i++) {
			if (!mount_is_direct_child(mount, directory, &entries[i])) continue;
			entries[i].info = mount->root_info;
			entries[i].info.flags |= FILESYSTEM_NODE_MOUNT_POINT;
			if (mount->read_only) entries[i].info.flags |= FILESYSTEM_NODE_READ_ONLY;
			break;
		}
	}
}

static bool reply_open_directory(const struct cap_request* call, const char* path, size_t path_size, cap_id_t raw_cap,
                                 uint64_t mount_id, bool read_only, cap_rights_t rights,
                                 struct filesystem_node_info info) {
	struct vfs_directory*           directory;
	struct filesystem_open_response response;
	cap_id_t                        published_cap = CAP_ID_INVALID;
	syscall_status_t                status;

	directory = calloc(1u, sizeof(*directory));
	if (directory == NULL) {
		if (raw_cap != CAP_ID_INVALID) (void)cap_drop(raw_cap);
		return reply_transport(call, SYSCALL_STATUS_FAILED);
	}
	directory->path = path_copy(path, path_size);
	if (directory->path == NULL) {
		if (raw_cap != CAP_ID_INVALID) (void)cap_drop(raw_cap);
		free(directory);
		return reply_transport(call, SYSCALL_STATUS_FAILED);
	}
	directory->path_size = path_size;
	directory->raw_cap   = raw_cap;
	directory->mount_id  = mount_id;
	directory->read_only = read_only;
	directory->attached  = true;
	if (directory_object_id(directory) == 0u || directory_object_id(directory) == VFS_SERVICE_OBJECT_ID) {
		release_directory(directory);
		return reply_transport(call, SYSCALL_STATUS_FAILED);
	}
	status = cap_publish(vfs_endpoint, directory_object_id(directory), call->caller, CAP_CALL | rights, &published_cap);
	if (status != SYSCALL_STATUS_OK) {
		release_directory(directory);
		return reply_transport(call, status);
	}
	directory->next = directories;
	directories     = directory;
	response        = (struct filesystem_open_response){
		.header     = {.status = FILESYSTEM_STATUS_OK, .reserved = 0u},
		.info       = info,
		.capability = published_cap,
	};
	if (reply_request(call->call_id, &response, sizeof(response), SYSCALL_STATUS_OK)) return true;
	(void)cap_unpublish(vfs_endpoint, directory_object_id(directory));
	unlink_directory(directory);
	release_directory(directory);
	return false;
}

static bool reply_open_file(const struct cap_request* call, cap_id_t raw_cap, cap_rights_t rights,
                            struct filesystem_node_info info) {
	struct filesystem_open_response response;
	cap_id_t                        delegated = CAP_ID_INVALID;
	syscall_status_t                status;

	status = cap_delegate(raw_cap, call->caller, CAP_CALL | rights, &delegated);
	if (status == SYSCALL_STATUS_OK) {
		syscall_status_t drop_status = cap_drop(raw_cap);
		if (drop_status != SYSCALL_STATUS_OK) status = drop_status;
	}
	else {
		(void)cap_drop(raw_cap);
	}
	if (status != SYSCALL_STATUS_OK) {
		if (delegated != CAP_ID_INVALID) (void)cap_revoke(delegated, 0u);
		return reply_transport(call, status);
	}
	response = (struct filesystem_open_response){
		.header     = {.status = FILESYSTEM_STATUS_OK, .reserved = 0u},
		.info       = info,
		.capability = delegated,
	};
	if (reply_request(call->call_id, &response, sizeof(response), SYSCALL_STATUS_OK)) return true;
	(void)cap_revoke(delegated, 0u);
	return false;
}

static bool reply_open_node(const struct cap_request* call, const char* path, size_t path_size, cap_rights_t rights,
                            struct filesystem_node_handle* node, struct vfs_mount* mount) {
	bool read_only = mount->read_only || (node->info.flags & FILESYSTEM_NODE_READ_ONLY) != 0u;
	apply_mount_flags(path, path_size, read_only, &node->info);
	if (node->info.type == FILESYSTEM_NODE_FILE) return reply_open_file(call, node->capability, rights, node->info);
	return reply_open_directory(call, path, path_size, node->capability, mount->id, read_only, rights, node->info);
}

static struct filesystem_call_result open_raw_node(struct vfs_mount* mount, const char* path, size_t path_size,
                                                   cap_rights_t rights, struct filesystem_node_handle* out_node) {
	const void*      relative;
	size_t           relative_size;
	syscall_status_t status;

	*out_node = (struct filesystem_node_handle){.capability = CAP_ID_INVALID};
	relative_path(mount, path, path_size, &relative, &relative_size);
	if (relative_size != 0u) {
		return filesystem_directory_open(
			mount->root_cap, relative, relative_size, rights | CAP_READ | CAP_DELEGATE, out_node);
	}
	out_node->info = mount->root_info;
	status         = cap_delegate(mount->root_cap, vfs_pid, CAP_CALL | CAP_READ, &out_node->capability);
	return (struct filesystem_call_result){
		.status           = status == SYSCALL_STATUS_OK ? FILESYSTEM_STATUS_OK : FILESYSTEM_STATUS_NONE,
		.transport_status = status,
	};
}

static bool handle_open(const struct cap_request* call, const void* data) {
	struct vfs_open_request       request;
	struct vfs_mount*             mount;
	struct filesystem_node_handle node;
	struct filesystem_call_result result;
	const char*                   path;
	size_t                        expected_size;

	if (!request_has_rights(call, CAP_CALL | CAP_READ)) return reply_transport(call, SYSCALL_STATUS_DENIED);
	if (!copy_request(call, data, &request, sizeof(request)) || request.path_size == 0u ||
	    request.path_size > CAP_MAX_REQUEST_SIZE - sizeof(request) ||
	    (expected_size = sizeof(request) + request.path_size) != call->request_size ||
	    call->response_capacity < sizeof(struct filesystem_open_response) || !node_rights_valid(request.rights))
		return reply_transport(call, SYSCALL_STATUS_BAD_ARGUMENT);
	path = (const char*)data + sizeof(request);
	if (!absolute_path_valid(path, request.path_size)) return reply_transport(call, SYSCALL_STATUS_BAD_ARGUMENT);

	mount = find_mount(path, request.path_size);
	if (mount == NULL) {
		struct filesystem_node_info info = {
			.type  = FILESYSTEM_NODE_DIRECTORY,
			.flags = FILESYSTEM_NODE_READ_ONLY,
			.size  = 0u,
		};
		if (request.path_size != 1u) return reply_domain(call, FILESYSTEM_STATUS_NOT_FOUND);
		return reply_open_directory(call, path, request.path_size, CAP_ID_INVALID, 0u, true, request.rights, info);
	}
	if (mount->read_only && (request.rights & CAP_WRITE) != 0u) return reply_domain(call, FILESYSTEM_STATUS_READ_ONLY);
	result = open_raw_node(mount, path, request.path_size, request.rights, &node);
	if (result.transport_status != SYSCALL_STATUS_OK) return reply_call_failure(call, result);
	if (result.status != FILESYSTEM_STATUS_OK) return reply_call_failure(call, result);
	return reply_open_node(call, path, request.path_size, request.rights, &node, mount);
}

static bool handle_create(const struct cap_request* call, const void* data, uint32_t expected_type) {
	struct vfs_open_request       request;
	struct vfs_mount*             mount;
	struct filesystem_node_handle node = {.capability = CAP_ID_INVALID};
	struct filesystem_call_result result;
	const char*                   path;
	const void*                   relative;
	size_t                        expected_size;
	size_t                        relative_size;
	cap_rights_t                  raw_rights;

	if (!request_has_rights(call, CAP_CALL | CAP_WRITE)) return reply_transport(call, SYSCALL_STATUS_DENIED);
	if (!copy_request(call, data, &request, sizeof(request)) || request.path_size == 0u ||
	    request.path_size > CAP_MAX_REQUEST_SIZE - sizeof(request) ||
	    (expected_size = sizeof(request) + request.path_size) != call->request_size ||
	    call->response_capacity < sizeof(struct filesystem_open_response) || !node_rights_valid(request.rights))
		return reply_transport(call, SYSCALL_STATUS_BAD_ARGUMENT);
	path = (const char*)data + sizeof(request);
	if (!absolute_path_valid(path, request.path_size)) return reply_transport(call, SYSCALL_STATUS_BAD_ARGUMENT);
	if (request.path_size == 1u || find_exact_mount(path, request.path_size) != NULL)
		return reply_domain(call, FILESYSTEM_STATUS_ALREADY_EXISTS);
	mount = find_mount(path, request.path_size);
	if (mount == NULL || mount->read_only) return reply_domain(call, FILESYSTEM_STATUS_READ_ONLY);
	relative_path(mount, path, request.path_size, &relative, &relative_size);
	if (relative_size == 0u) return reply_domain(call, FILESYSTEM_STATUS_ALREADY_EXISTS);
	raw_rights = expected_type == FILESYSTEM_NODE_FILE ? request.rights | CAP_DELEGATE : CAP_READ;
	if (expected_type == FILESYSTEM_NODE_FILE) {
		result = filesystem_directory_create_file(mount->root_cap, relative, relative_size, raw_rights, &node);
	}
	else {
		result = filesystem_directory_create_directory(mount->root_cap, relative, relative_size, raw_rights, &node);
	}
	if (result.transport_status != SYSCALL_STATUS_OK || result.status != FILESYSTEM_STATUS_OK)
		return reply_call_failure(call, result);
	return reply_open_node(call, path, request.path_size, request.rights, &node, mount);
}

static void detach_directories(uint64_t mount_id, const char* path, size_t path_size) {
	for (struct vfs_directory* directory = directories; directory != NULL; directory = directory->next) {
		if (directory->attached && directory->mount_id == mount_id &&
		    path_contains(path, path_size, directory->path, directory->path_size))
			directory->attached = false;
	}
}

static bool handle_remove(const struct cap_request* call, const void* data) {
	struct vfs_path_request       request;
	struct vfs_mount*             mount;
	struct filesystem_call_result result;
	const char*                   path;
	const void*                   relative;
	size_t                        expected_size;
	size_t                        relative_size;

	if (!request_has_rights(call, CAP_CALL | CAP_WRITE)) return reply_transport(call, SYSCALL_STATUS_DENIED);
	if (!copy_request(call, data, &request, sizeof(request)) || request.path_size == 0u ||
	    request.path_size > CAP_MAX_REQUEST_SIZE - sizeof(request) ||
	    (expected_size = sizeof(request) + request.path_size) != call->request_size ||
	    call->response_capacity < sizeof(struct filesystem_response_header))
		return reply_transport(call, SYSCALL_STATUS_BAD_ARGUMENT);
	path = (const char*)data + sizeof(request);
	if (!absolute_path_valid(path, request.path_size)) return reply_transport(call, SYSCALL_STATUS_BAD_ARGUMENT);
	if (request.path_size == 1u || mount_at_or_below(path, request.path_size))
		return reply_domain(call, FILESYSTEM_STATUS_BUSY);
	mount = find_mount(path, request.path_size);
	if (mount == NULL || mount->read_only) return reply_domain(call, FILESYSTEM_STATUS_READ_ONLY);
	relative_path(mount, path, request.path_size, &relative, &relative_size);
	result = filesystem_directory_remove(mount->root_cap, relative, relative_size);
	if (result.transport_status != SYSCALL_STATUS_OK || result.status != FILESYSTEM_STATUS_OK)
		return reply_call_failure(call, result);
	detach_directories(mount->id, path, request.path_size);
	return reply_domain(call, FILESYSTEM_STATUS_OK);
}

static void free_rename_updates(struct rename_update* updates) {
	while (updates != NULL) {
		struct rename_update* next = updates->next;
		free(updates->path);
		free(updates);
		updates = next;
	}
}

static struct rename_update* prepare_rename_updates(uint64_t mount_id, const char* old_path, size_t old_path_size,
                                                    const char* new_path, size_t new_path_size) {
	struct rename_update* updates = NULL;

	for (struct vfs_directory* directory = directories; directory != NULL; directory = directory->next) {
		struct rename_update* update;
		size_t                suffix_size;
		size_t                replacement_size;
		if (!directory->attached || directory->mount_id != mount_id ||
		    !path_contains(old_path, old_path_size, directory->path, directory->path_size))
			continue;
		suffix_size = directory->path_size - old_path_size;
		if (new_path_size > SIZE_MAX - suffix_size) {
			free_rename_updates(updates);
			return NULL;
		}
		replacement_size = new_path_size + suffix_size;
		update           = calloc(1u, sizeof(*update));
		if (update != NULL) update->path = malloc(replacement_size);
		if (update == NULL || update->path == NULL) {
			if (update != NULL) free(update);
			free_rename_updates(updates);
			return NULL;
		}
		memcpy(update->path, new_path, new_path_size);
		memcpy(update->path + new_path_size, directory->path + old_path_size, suffix_size);
		update->directory = directory;
		update->path_size = replacement_size;
		update->next      = updates;
		updates           = update;
	}
	return updates;
}

static void apply_rename_updates(struct rename_update* updates) {
	while (updates != NULL) {
		struct rename_update* next = updates->next;
		free(updates->directory->path);
		updates->directory->path      = updates->path;
		updates->directory->path_size = updates->path_size;
		free(updates);
		updates = next;
	}
}

static bool handle_rename(const struct cap_request* call, const void* data) {
	struct vfs_rename_request     request;
	struct vfs_mount*             old_mount;
	struct vfs_mount*             new_mount;
	struct filesystem_call_result result;
	struct rename_update*         updates;
	const char*                   old_path;
	const char*                   new_path;
	const void*                   old_relative;
	const void*                   new_relative;
	size_t                        expected_size;
	size_t                        old_relative_size;
	size_t                        new_relative_size;

	if (!request_has_rights(call, CAP_CALL | CAP_WRITE)) return reply_transport(call, SYSCALL_STATUS_DENIED);
	if (!copy_request(call, data, &request, sizeof(request)) || request.reserved != 0u || request.old_path_size == 0u ||
	    request.new_path_size == 0u || request.old_path_size > CAP_MAX_REQUEST_SIZE - sizeof(request) ||
	    request.new_path_size > CAP_MAX_REQUEST_SIZE - sizeof(request) - request.old_path_size ||
	    (expected_size = sizeof(request) + request.old_path_size + request.new_path_size) != call->request_size ||
	    call->response_capacity < sizeof(struct filesystem_response_header))
		return reply_transport(call, SYSCALL_STATUS_BAD_ARGUMENT);
	old_path = (const char*)data + sizeof(request);
	new_path = old_path + request.old_path_size;
	if (!absolute_path_valid(old_path, request.old_path_size) || !absolute_path_valid(new_path, request.new_path_size))
		return reply_transport(call, SYSCALL_STATUS_BAD_ARGUMENT);
	if (request.old_path_size == 1u || request.new_path_size == 1u ||
	    mount_at_or_below(old_path, request.old_path_size) || mount_at_or_below(new_path, request.new_path_size))
		return reply_domain(call, FILESYSTEM_STATUS_BUSY);
	old_mount = find_mount(old_path, request.old_path_size);
	new_mount = find_mount(new_path, request.new_path_size);
	if (old_mount != new_mount) return reply_domain(call, FILESYSTEM_STATUS_CROSS_FILESYSTEM);
	if (old_mount == NULL || old_mount->read_only) return reply_domain(call, FILESYSTEM_STATUS_READ_ONLY);
	relative_path(old_mount, old_path, request.old_path_size, &old_relative, &old_relative_size);
	relative_path(new_mount, new_path, request.new_path_size, &new_relative, &new_relative_size);
	updates = prepare_rename_updates(old_mount->id, old_path, request.old_path_size, new_path, request.new_path_size);
	if (updates == NULL) {
		bool needs_update = false;
		for (struct vfs_directory* directory = directories; directory != NULL; directory = directory->next) {
			if (directory->attached && directory->mount_id == old_mount->id &&
			    path_contains(old_path, request.old_path_size, directory->path, directory->path_size)) {
				needs_update = true;
				break;
			}
		}
		if (needs_update) return reply_transport(call, SYSCALL_STATUS_FAILED);
	}
	result = filesystem_directory_rename(
		old_mount->root_cap, old_relative, old_relative_size, new_relative, new_relative_size);
	if (result.transport_status != SYSCALL_STATUS_OK || result.status != FILESYSTEM_STATUS_OK) {
		free_rename_updates(updates);
		return reply_call_failure(call, result);
	}
	apply_rename_updates(updates);
	return reply_domain(call, FILESYSTEM_STATUS_OK);
}

static bool handle_directory_info(const struct cap_request* call, struct vfs_directory* directory, const void* data) {
	struct filesystem_info_request  request;
	struct filesystem_info_response response;
	struct filesystem_call_result   result;

	if (!request_has_rights(call, CAP_CALL | CAP_READ)) return reply_transport(call, SYSCALL_STATUS_DENIED);
	if (!copy_request(call, data, &request, sizeof(request)) || call->request_size != sizeof(request) ||
	    request.reserved != 0u || call->response_capacity < sizeof(response))
		return reply_transport(call, SYSCALL_STATUS_BAD_ARGUMENT);
	if (directory->raw_cap == CAP_ID_INVALID) {
		response.info = (struct filesystem_node_info){
			.type  = FILESYSTEM_NODE_DIRECTORY,
			.flags = FILESYSTEM_NODE_READ_ONLY,
			.size  = 0u,
		};
	}
	else {
		result = filesystem_directory_info(directory->raw_cap, &response.info);
		if (result.transport_status != SYSCALL_STATUS_OK || result.status != FILESYSTEM_STATUS_OK)
			return reply_call_failure(call, result);
	}
	if (directory->attached)
		apply_mount_flags(directory->path, directory->path_size, directory->read_only, &response.info);
	else if (directory->read_only) response.info.flags |= FILESYSTEM_NODE_READ_ONLY;
	response.header = (struct filesystem_response_header){.status = FILESYSTEM_STATUS_OK, .reserved = 0u};
	return reply_request(call->call_id, &response, sizeof(response), SYSCALL_STATUS_OK);
}

static bool handle_directory_enumerate(const struct cap_request* call, struct vfs_directory* directory,
                                       const void* data) {
	struct filesystem_directory_enumerate_request   request;
	struct filesystem_directory_enumerate_response* response;
	struct filesystem_call_result                   result;
	size_t                                          capacity;
	size_t                                          maximum_count;
	size_t                                          returned = 0u;
	uint64_t                                        total    = 0u;

	if (!request_has_rights(call, CAP_CALL | CAP_READ)) return reply_transport(call, SYSCALL_STATUS_DENIED);
	maximum_count = (CAP_MAX_RESPONSE_SIZE - sizeof(*response)) / sizeof(struct filesystem_directory_entry);
	if (!copy_request(call, data, &request, sizeof(request)) || call->request_size != sizeof(request) ||
	    request.reserved != 0u || call->response_capacity < sizeof(*response))
		return reply_transport(call, SYSCALL_STATUS_BAD_ARGUMENT);
	if (request.count > maximum_count ||
	    request.count > (call->response_capacity - sizeof(*response)) / sizeof(struct filesystem_directory_entry))
		return reply_transport(call, SYSCALL_STATUS_BAD_ARGUMENT);
	capacity = sizeof(*response) + (size_t)request.count * sizeof(struct filesystem_directory_entry);
	response = malloc(capacity);
	if (response == NULL) return reply_transport(call, SYSCALL_STATUS_FAILED);
	if (directory->raw_cap != CAP_ID_INVALID) {
		result = filesystem_directory_enumerate(
			directory->raw_cap, request.offset, response->entries, (size_t)request.count, &returned, &total);
		if (result.transport_status != SYSCALL_STATUS_OK || result.status != FILESYSTEM_STATUS_OK) {
			free(response);
			return reply_call_failure(call, result);
		}
	}
	patch_enumeration(directory, response->entries, returned);
	response->header   = (struct filesystem_response_header){.status = FILESYSTEM_STATUS_OK, .reserved = 0u};
	response->total    = total;
	response->returned = returned;
	bool replied       = reply_request(call->call_id,
	                                   response,
	                                   sizeof(*response) + returned * sizeof(struct filesystem_directory_entry),
	                                   SYSCALL_STATUS_OK);
	free(response);
	return replied;
}

static uint64_t allocate_mount_id(void) {
	uint64_t id = next_mount_id++;
	if (id == 0u) id = next_mount_id++;
	return id;
}

static bool handle_directory_mount(const struct cap_request* call, struct vfs_directory* directory, const void* data) {
	struct vfs_directory_mount_request request;
	struct filesystem_node_info        info;
	struct filesystem_call_result      result;
	struct vfs_mount*                  mount;
	cap_id_t                           probe = CAP_ID_INVALID;
	syscall_status_t                   status;

	if (!request_has_rights(call, CAP_CALL | CAP_MANAGE)) return reply_transport(call, SYSCALL_STATUS_DENIED);
	if (!copy_request(call, data, &request, sizeof(request)) || call->request_size != sizeof(request) ||
	    call->response_capacity < sizeof(struct filesystem_response_header) ||
	    (request.flags & ~VFS_MOUNT_READ_ONLY) != 0u || request.root_capability == CAP_ID_INVALID)
		return reply_transport(call, SYSCALL_STATUS_BAD_ARGUMENT);
	if (!directory->attached) return reply_domain(call, FILESYSTEM_STATUS_BUSY);
	if (find_exact_mount(directory->path, directory->path_size) != NULL)
		return reply_domain(call, FILESYSTEM_STATUS_ALREADY_EXISTS);
	result = filesystem_directory_info(request.root_capability, &info);
	if (result.transport_status != SYSCALL_STATUS_OK || result.status != FILESYSTEM_STATUS_OK)
		return reply_call_failure(call, result);
	status = cap_delegate(request.root_capability, vfs_pid, CAP_CALL | CAP_READ, &probe);
	if (status == SYSCALL_STATUS_OK) status = cap_drop(probe);
	if (status != SYSCALL_STATUS_OK) return reply_transport(call, status);
	mount = calloc(1u, sizeof(*mount));
	if (mount == NULL) return reply_transport(call, SYSCALL_STATUS_FAILED);
	mount->path = path_copy(directory->path, directory->path_size);
	if (mount->path == NULL) {
		free(mount);
		return reply_transport(call, SYSCALL_STATUS_FAILED);
	}
	mount->id        = allocate_mount_id();
	mount->path_size = directory->path_size;
	mount->root_cap  = request.root_capability;
	mount->root_info = info;
	mount->read_only = (request.flags & VFS_MOUNT_READ_ONLY) != 0u || (info.flags & FILESYSTEM_NODE_READ_ONLY) != 0u;
	insert_mount(mount);
	if (reply_domain(call, FILESYSTEM_STATUS_OK)) return true;
	unlink_mount(mount);
	free(mount->path);
	free(mount);
	return false;
}

static bool handle_directory_unmount(const struct cap_request* call, struct vfs_directory* directory,
                                     const void* data) {
	struct filesystem_info_request request;
	struct vfs_mount*              mount;
	syscall_status_t               status;

	if (!request_has_rights(call, CAP_CALL | CAP_MANAGE)) return reply_transport(call, SYSCALL_STATUS_DENIED);
	if (!copy_request(call, data, &request, sizeof(request)) || call->request_size != sizeof(request) ||
	    request.reserved != 0u || call->response_capacity < sizeof(struct filesystem_response_header))
		return reply_transport(call, SYSCALL_STATUS_BAD_ARGUMENT);
	if (!directory->attached) return reply_domain(call, FILESYSTEM_STATUS_BUSY);
	mount = find_exact_mount(directory->path, directory->path_size);
	if (mount == NULL) return reply_domain(call, FILESYSTEM_STATUS_NOT_FOUND);
	if (mount_has_children(mount)) return reply_domain(call, FILESYSTEM_STATUS_BUSY);
	status = cap_drop(mount->root_cap);
	if (status != SYSCALL_STATUS_OK) return reply_transport(call, status);
	unlink_mount(mount);
	for (struct vfs_directory* item = directories; item != NULL; item = item->next) {
		if (item->mount_id == mount->id) item->attached = false;
	}
	free(mount->path);
	free(mount);
	return reply_domain(call, FILESYSTEM_STATUS_OK);
}

static bool dispatch_service(const struct cap_request* call, const void* data,
                             const struct filesystem_request_header* header) {
	switch (header->op) {
	case VFS_OP_OPEN:
		return handle_open(call, data);
	case VFS_OP_CREATE_FILE:
		return handle_create(call, data, FILESYSTEM_NODE_FILE);
	case VFS_OP_CREATE_DIRECTORY:
		return handle_create(call, data, FILESYSTEM_NODE_DIRECTORY);
	case VFS_OP_REMOVE:
		return handle_remove(call, data);
	case VFS_OP_RENAME:
		return handle_rename(call, data);
	default:
		return reply_transport(call, SYSCALL_STATUS_BAD_ARGUMENT);
	}
}

static bool dispatch_directory(const struct cap_request* call, struct vfs_directory* directory, const void* data,
                               const struct filesystem_request_header* header) {
	switch (header->op) {
	case VFS_DIRECTORY_OP_INFO:
		return handle_directory_info(call, directory, data);
	case VFS_DIRECTORY_OP_ENUMERATE:
		return handle_directory_enumerate(call, directory, data);
	case VFS_DIRECTORY_OP_MOUNT:
		return handle_directory_mount(call, directory, data);
	case VFS_DIRECTORY_OP_UNMOUNT:
		return handle_directory_unmount(call, directory, data);
	default:
		return reply_transport(call, SYSCALL_STATUS_BAD_ARGUMENT);
	}
}

static bool dispatch_request(const struct cap_request* call, const void* data) {
	struct filesystem_request_header header;
	struct vfs_directory*            directory;

	if (!request_has_rights(call, CAP_CALL)) return reply_transport(call, SYSCALL_STATUS_DENIED);
	if (!copy_request(call, data, &header, sizeof(header))) return reply_transport(call, SYSCALL_STATUS_BAD_ARGUMENT);
	if (call->object_id == VFS_SERVICE_OBJECT_ID) return dispatch_service(call, data, &header);
	directory = find_directory(call->object_id);
	if (directory == NULL) return reply_transport(call, SYSCALL_STATUS_BAD_ARGUMENT);
	return dispatch_directory(call, directory, data, &header);
}

static bool advertise_vfs(void) {
	const struct init_service_selector selector = {
		.namespace_path = VFS_NAMESPACE,
		.protocol       = VFS_PROTOCOL_NAME,
		.major          = VFS_PROTOCOL_VERSION_MAJOR,
		.service        = VFS_SERVICE_NAME,
	};
	struct self_info        self;
	struct init_call_result result;
	syscall_status_t        status;

	status = process_self_info(&self);
	if (status != SYSCALL_STATUS_OK || self.pid == PROCESS_PID_INVALID) return false;
	vfs_pid = self.pid;
	status  = channel_create(&vfs_endpoint, &vfs_activity);
	if (status != SYSCALL_STATUS_OK) return false;
	status = cap_publish(
		vfs_endpoint, VFS_SERVICE_OBJECT_ID, vfs_pid, VFS_SERVICE_CLIENT_RIGHTS | CAP_DELEGATE, &vfs_service_cap);
	if (status != SYSCALL_STATUS_OK) return false;
	result = init_advertise(&selector, VFS_PROTOCOL_VERSION_MINOR, vfs_service_cap, VFS_SERVICE_CLIENT_RIGHTS);
	return result.status == INIT_REGISTRY_OK && result.transport_status == SYSCALL_STATUS_OK;
}

static void handle_channel_event(const struct channel_event* event) {
	struct vfs_directory* directory;

	if (event->type != CHANNEL_EVENT_CAP_ZERO_GRANTS || event->object_id == VFS_SERVICE_OBJECT_ID) return;
	directory = find_directory(event->object_id);
	if (directory == NULL || cap_unpublish_if_unused(vfs_endpoint, event->object_id) != SYSCALL_STATUS_OK) return;
	unlink_directory(directory);
	release_directory(directory);
}

int vfs_server_run(void) {
	struct cap_request call;
	bool               received;
	syscall_status_t   status;

	if (!advertise_vfs()) {
		printf("vfs: advertisement failed\n");
		return 1;
	}
	printf("vfs: advertised %s/%s@%u.%u/%s\n",
	       VFS_NAMESPACE,
	       VFS_PROTOCOL_NAME,
	       VFS_PROTOCOL_VERSION_MAJOR,
	       VFS_PROTOCOL_VERSION_MINOR,
	       VFS_SERVICE_NAME);

	for (;;) {
		do {
			received = false;
			status   = channel_recv(vfs_endpoint, &call, request_buffer, sizeof(request_buffer), &received);
			if (status != SYSCALL_STATUS_OK) {
				printf("vfs: channel receive failed: %u\n", (unsigned)status);
				return 1;
			}
			if (received && !dispatch_request(&call, request_buffer)) printf("vfs: channel reply failed\n");
		} while (received);
		do {
			struct channel_event event;
			received = false;
			status   = channel_event_recv(vfs_endpoint, &event, &received);
			if (status != SYSCALL_STATUS_OK) return 1;
			if (received) handle_channel_event(&event);
		} while (received);
		struct signal_message activity;
		status = signal_wait(vfs_activity, &activity);
		if (status != SYSCALL_STATUS_OK) return 1;
	}
}
