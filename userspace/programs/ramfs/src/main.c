#include <base/cap.h>
#include <base/process.h>
#include <protocol/filesystem.h>
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

#define RAMFS_SERVICE_NAME "ram"
#define FILESYSTEM_PROTOCOL_VERSION_MAJOR 1u
#define FILESYSTEM_PROTOCOL_VERSION_MINOR 1u
#define RAMFS_ROOT_OBJECT_ID 1u
#define RAMFS_CLIENT_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_READ | CAP_WRITE | CAP_MANAGE | CAP_DELEGATE))
#define RAMFS_SOURCE_RIGHTS RAMFS_CLIENT_RIGHTS

/* One file, directory or symbolic link; attached nodes and open handles each hold a reference. */
struct ramfs_node {
	uint32_t           type;
	size_t             references;
	struct ramfs_node* parent;
	struct ramfs_node* children;
	struct ramfs_node* next;
	size_t             child_count;
	uint8_t*           name;
	size_t             name_size;
	uint8_t*           data;
	size_t             size;
};

struct ramfs_handle {
	struct ramfs_node*   node;
	struct ramfs_handle* next;
};

static channel_id_t         ramfs_endpoint = CHANNEL_ID_INVALID;
static cap_id_t             ramfs_activity = CAP_ID_INVALID;
static cap_id_t             ramfs_root_cap = CAP_ID_INVALID;
static struct ramfs_node    root           = {.type = FILESYSTEM_NODE_DIRECTORY, .references = 1u};
static struct ramfs_handle* handles;
static uint8_t              request_buffer[CAP_MAX_REQUEST_SIZE];
static uint8_t              response_buffer[CAP_MAX_RESPONSE_SIZE];

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

static bool request_has_rights(const struct cap_request* call, cap_rights_t rights) {
	return (call->rights & rights) == rights;
}

static bool copy_request(const struct cap_request* call, const void* data, void* out, size_t size) {
	if (call == NULL || data == NULL || out == NULL || call->request_size < size) return false;
	memcpy(out, data, size);
	return true;
}

/* Requested node rights must be valid and never exceed the rights of the calling capability. */
static bool node_rights_valid(const struct cap_request* call, cap_rights_t rights) {
	return (rights & ~FILESYSTEM_NODE_REQUEST_RIGHTS) == 0u && (rights & ~call->rights) == 0u;
}

static struct filesystem_node_info node_info(const struct ramfs_node* node) {
	return (struct filesystem_node_info){
		.type  = node->type,
		.flags = 0u,
		.size  = node->type == FILESYSTEM_NODE_DIRECTORY ? 0u : node->size,
	};
}

static bool node_detached(const struct ramfs_node* node) {
	return node != &root && node->parent == NULL;
}

static void release_node(struct ramfs_node* node) {
	if (node == &root || --node->references != 0u) return;
	free(node->name);
	free(node->data);
	free(node);
}

static struct ramfs_node* find_child(const struct ramfs_node* directory, const uint8_t* name, size_t name_size) {
	for (struct ramfs_node* child = directory->children; child != NULL; child = child->next) {
		if (child->name_size == name_size && memcmp(child->name, name, name_size) == 0) return child;
	}
	return NULL;
}

static void attach_node(struct ramfs_node* parent, struct ramfs_node* node) {
	struct ramfs_node** cursor = &parent->children;
	while (*cursor != NULL) cursor = &(*cursor)->next;
	node->next   = NULL;
	node->parent = parent;
	*cursor      = node;
	parent->child_count++;
}

static void unlink_node(struct ramfs_node* node) {
	struct ramfs_node** cursor = &node->parent->children;
	while (*cursor != node) cursor = &(*cursor)->next;
	*cursor = node->next;
	node->parent->child_count--;
	node->parent = NULL;
	node->next   = NULL;
}

static struct ramfs_node* create_node(struct ramfs_node* parent, uint32_t type, const uint8_t* name, size_t name_size) {
	struct ramfs_node* node = calloc(1u, sizeof(*node));
	if (node == NULL) return NULL;
	node->name = malloc(name_size);
	if (node->name == NULL) {
		free(node);
		return NULL;
	}
	memcpy(node->name, name, name_size);
	node->name_size  = name_size;
	node->type       = type;
	node->references = 1u;
	attach_node(parent, node);
	return node;
}

static bool resize_node(struct ramfs_node* node, uint64_t size) {
	uint8_t* data;

	if (size > SIZE_MAX) return false;
	if ((size_t)size > node->size) {
		data = realloc(node->data, (size_t)size);
		if (data == NULL) return false;
		memset(data + node->size, 0, (size_t)size - node->size);
		node->data = data;
	}
	node->size = (size_t)size;
	return true;
}

/*
 * Walk a validated relative path below a directory. With parent_only, stop
 * before the final component and report its name instead of looking it up.
 * Symbolic links are never followed.
 */
static enum filesystem_status walk(struct ramfs_node* base, const uint8_t* path, size_t path_size, bool parent_only,
                                   struct ramfs_node** out_node, const uint8_t** out_name, size_t* out_name_size) {
	struct ramfs_node* node  = base;
	size_t             start = 0u;

	for (size_t i = 0u; i <= path_size; i++) {
		const uint8_t*     name;
		size_t             name_size;
		struct ramfs_node* child;
		bool               final = i == path_size;

		if (!final && path[i] != '/') continue;
		name      = path + start;
		name_size = i - start;
		start     = i + 1u;
		if (node_detached(node)) return FILESYSTEM_STATUS_NOT_FOUND;
		if (final && parent_only) {
			*out_node      = node;
			*out_name      = name;
			*out_name_size = name_size;
			return FILESYSTEM_STATUS_OK;
		}
		child = find_child(node, name, name_size);
		if (child == NULL) return FILESYSTEM_STATUS_NOT_FOUND;
		if (!final && child->type == FILESYSTEM_NODE_SYMBOLIC_LINK) return FILESYSTEM_STATUS_SYMBOLIC_LINK;
		if (!final && child->type != FILESYSTEM_NODE_DIRECTORY) return FILESYSTEM_STATUS_NOT_DIRECTORY;
		node = child;
	}
	*out_node = node;
	return FILESYSTEM_STATUS_OK;
}

static uint64_t handle_object_id(const struct ramfs_handle* handle) {
	return (uint64_t)(uintptr_t)handle;
}

static struct ramfs_handle* find_handle(uint64_t object_id) {
	for (struct ramfs_handle* handle = handles; handle != NULL; handle = handle->next) {
		if (handle_object_id(handle) == object_id) return handle;
	}
	return NULL;
}

static void unlink_handle(struct ramfs_handle* target) {
	struct ramfs_handle** cursor = &handles;
	while (*cursor != NULL) {
		if (*cursor == target) {
			*cursor      = target->next;
			target->next = NULL;
			return;
		}
		cursor = &(*cursor)->next;
	}
}

static void release_handle(struct ramfs_handle* handle) {
	if (handle == NULL) return;
	release_node(handle->node);
	free(handle);
}

static bool reply_open_node(const struct cap_request* call, struct ramfs_node* node, cap_rights_t rights) {
	struct filesystem_open_response response = {
		.header = {.status = FILESYSTEM_STATUS_OK, .reserved = 0u},
		.info   = node_info(node),
	};
	struct ramfs_handle* handle;
	syscall_status_t     status;

	handle = calloc(1u, sizeof(*handle));
	if (handle == NULL) return reply_transport(call, SYSCALL_STATUS_FAILED);
	handle->node = node;
	node->references++;
	if (handle_object_id(handle) == 0u || handle_object_id(handle) == RAMFS_ROOT_OBJECT_ID) {
		release_handle(handle);
		return reply_transport(call, SYSCALL_STATUS_FAILED);
	}
	status =
		cap_derive(ramfs_root_cap, call->caller, handle_object_id(handle), CAP_CALL | rights, &response.capability);
	if (status != SYSCALL_STATUS_OK) {
		release_handle(handle);
		return reply_transport(call, status);
	}
	handle->next = handles;
	handles      = handle;
	if (reply_request(call->call_id, &response, sizeof(response), SYSCALL_STATUS_OK)) return true;
	(void)cap_unpublish(ramfs_endpoint, handle_object_id(handle));
	unlink_handle(handle);
	release_handle(handle);
	return false;
}

static bool handle_node_info(const struct cap_request* call, const void* data, const struct ramfs_node* node) {
	struct filesystem_info_request  request;
	struct filesystem_info_response response = {
		.header = {.status = FILESYSTEM_STATUS_OK, .reserved = 0u},
		.info   = node_info(node),
	};

	if (!request_has_rights(call, CAP_CALL | CAP_READ)) return reply_transport(call, SYSCALL_STATUS_DENIED);
	if (!copy_request(call, data, &request, sizeof(request)) || call->request_size != sizeof(request) ||
	    request.reserved != 0u || call->response_capacity < sizeof(response))
		return reply_transport(call, SYSCALL_STATUS_BAD_ARGUMENT);
	return reply_request(call->call_id, &response, sizeof(response), SYSCALL_STATUS_OK);
}

/* Validate an open-shaped request (OPEN and creation) and return its path. */
static const uint8_t* open_request_path(const struct cap_request* call, const void* data, cap_rights_t required,
                                        struct filesystem_directory_open_request* out_request, bool* out_replied) {
	const uint8_t* path;

	*out_replied = true;
	if (!request_has_rights(call, CAP_CALL | required)) {
		*out_replied = reply_transport(call, SYSCALL_STATUS_DENIED);
		return NULL;
	}
	if (!copy_request(call, data, out_request, sizeof(*out_request)) || out_request->path_size == 0u ||
	    out_request->path_size > CAP_MAX_REQUEST_SIZE - sizeof(*out_request) ||
	    sizeof(*out_request) + out_request->path_size != call->request_size ||
	    call->response_capacity < sizeof(struct filesystem_open_response) ||
	    !node_rights_valid(call, out_request->rights) ||
	    !filesystem_relative_path_valid((path = (const uint8_t*)data + sizeof(*out_request)), out_request->path_size)) {
		*out_replied = reply_transport(call, SYSCALL_STATUS_BAD_ARGUMENT);
		return NULL;
	}
	return path;
}

/* Validate a filesystem_directory_path_request and return its path. */
static const uint8_t* path_request_path(const struct cap_request* call, const void* data, cap_rights_t required,
                                        size_t response_size, size_t* out_size, bool* out_replied) {
	struct filesystem_directory_path_request request;
	const uint8_t*                           path;

	*out_replied = true;
	if (!request_has_rights(call, CAP_CALL | required)) {
		*out_replied = reply_transport(call, SYSCALL_STATUS_DENIED);
		return NULL;
	}
	if (!copy_request(call, data, &request, sizeof(request)) || request.path_size == 0u ||
	    request.path_size > CAP_MAX_REQUEST_SIZE - sizeof(request) ||
	    sizeof(request) + request.path_size != call->request_size || call->response_capacity < response_size ||
	    !filesystem_relative_path_valid((path = (const uint8_t*)data + sizeof(request)), request.path_size)) {
		*out_replied = reply_transport(call, SYSCALL_STATUS_BAD_ARGUMENT);
		return NULL;
	}
	*out_size = request.path_size;
	return path;
}

static bool handle_directory_open(const struct cap_request* call, const void* data, struct ramfs_node* directory) {
	struct filesystem_directory_open_request request;
	struct ramfs_node*                       node;
	enum filesystem_status                   status;
	const uint8_t*                           path;
	bool                                     replied;

	path = open_request_path(call, data, CAP_READ, &request, &replied);
	if (path == NULL) return replied;
	status = walk(directory, path, request.path_size, false, &node, NULL, NULL);
	if (status != FILESYSTEM_STATUS_OK) return reply_domain(call, status);
	if (node->type == FILESYSTEM_NODE_SYMBOLIC_LINK) return reply_domain(call, FILESYSTEM_STATUS_SYMBOLIC_LINK);
	return reply_open_node(call, node, request.rights);
}

static bool handle_directory_create(const struct cap_request* call, const void* data, struct ramfs_node* directory,
                                    uint32_t type) {
	struct filesystem_directory_open_request request;
	struct ramfs_node*                       parent;
	struct ramfs_node*                       node;
	enum filesystem_status                   status;
	const uint8_t*                           path;
	const uint8_t*                           name;
	size_t                                   name_size;
	bool                                     replied;

	path = open_request_path(call, data, CAP_WRITE, &request, &replied);
	if (path == NULL) return replied;
	status = walk(directory, path, request.path_size, true, &parent, &name, &name_size);
	if (status != FILESYSTEM_STATUS_OK) return reply_domain(call, status);
	if (find_child(parent, name, name_size) != NULL) return reply_domain(call, FILESYSTEM_STATUS_ALREADY_EXISTS);
	node = create_node(parent, type, name, name_size);
	if (node == NULL) return reply_domain(call, FILESYSTEM_STATUS_NO_SPACE);
	return reply_open_node(call, node, request.rights);
}

static void fill_directory_entry(struct filesystem_directory_entry* entry, const struct ramfs_node* node) {
	*entry = (struct filesystem_directory_entry){
		.name_size = (uint32_t)node->name_size,
		.reserved  = 0u,
		.info      = node_info(node),
	};
	memcpy(entry->name, node->name, node->name_size);
}

static bool handle_directory_enumerate(const struct cap_request* call, const void* data,
                                       const struct ramfs_node* directory) {
	struct filesystem_directory_enumerate_request   request;
	struct filesystem_directory_enumerate_response* response = (void*)response_buffer;
	const size_t                                    maximum_count =
		(CAP_MAX_RESPONSE_SIZE - sizeof(*response)) / sizeof(struct filesystem_directory_entry);
	const struct ramfs_node* child    = directory->children;
	size_t                   returned = 0u;

	if (!request_has_rights(call, CAP_CALL | CAP_READ)) return reply_transport(call, SYSCALL_STATUS_DENIED);
	if (!copy_request(call, data, &request, sizeof(request)) || call->request_size != sizeof(request) ||
	    request.reserved != 0u || call->response_capacity < sizeof(*response) || request.count > maximum_count ||
	    request.count > (call->response_capacity - sizeof(*response)) / sizeof(struct filesystem_directory_entry))
		return reply_transport(call, SYSCALL_STATUS_BAD_ARGUMENT);
	for (uint64_t skipped = 0u; child != NULL && skipped < request.offset; skipped++) child = child->next;
	for (; child != NULL && returned < (size_t)request.count; child = child->next)
		fill_directory_entry(&response->entries[returned++], child);
	response->header   = (struct filesystem_response_header){.status = FILESYSTEM_STATUS_OK, .reserved = 0u};
	response->total    = directory->child_count;
	response->returned = returned;
	return reply_request(call->call_id,
	                     response,
	                     sizeof(*response) + returned * sizeof(struct filesystem_directory_entry),
	                     SYSCALL_STATUS_OK);
}

static bool handle_directory_lookup(const struct cap_request* call, const void* data, struct ramfs_node* directory) {
	struct filesystem_info_response response = {
		.header = {.status = FILESYSTEM_STATUS_OK, .reserved = 0u},
	};
	struct ramfs_node*     node;
	enum filesystem_status status;
	const uint8_t*         path;
	size_t                 path_size;
	bool                   replied;

	path = path_request_path(call, data, CAP_READ, sizeof(response), &path_size, &replied);
	if (path == NULL) return replied;
	status = walk(directory, path, path_size, false, &node, NULL, NULL);
	if (status != FILESYSTEM_STATUS_OK) return reply_domain(call, status);
	response.info = node_info(node);
	return reply_request(call->call_id, &response, sizeof(response), SYSCALL_STATUS_OK);
}

static bool handle_directory_read_symbolic_link(const struct cap_request* call, const void* data,
                                                struct ramfs_node* directory) {
	struct filesystem_symbolic_link_read_response* response = (void*)response_buffer;
	struct ramfs_node*                             node;
	enum filesystem_status                         status;
	const uint8_t*                                 path;
	size_t                                         path_size;
	bool                                           replied;

	path = path_request_path(call, data, CAP_READ, sizeof(*response), &path_size, &replied);
	if (path == NULL) return replied;
	status = walk(directory, path, path_size, false, &node, NULL, NULL);
	if (status != FILESYSTEM_STATUS_OK) return reply_domain(call, status);
	if (node->type != FILESYSTEM_NODE_SYMBOLIC_LINK) return reply_domain(call, FILESYSTEM_STATUS_INVALID_ARGUMENT);
	if (call->response_capacity < sizeof(*response) + node->size)
		return reply_transport(call, SYSCALL_STATUS_BAD_ARGUMENT);
	response->header = (struct filesystem_response_header){.status = FILESYSTEM_STATUS_OK, .reserved = 0u};
	memcpy(response->target, node->data, node->size);
	return reply_request(call->call_id, response, sizeof(*response) + node->size, SYSCALL_STATUS_OK);
}

static bool handle_directory_create_symbolic_link(const struct cap_request* call, const void* data,
                                                  struct ramfs_node* directory) {
	struct filesystem_directory_symbolic_link_request request;
	struct ramfs_node*                                parent;
	struct ramfs_node*                                node;
	enum filesystem_status                            status;
	const uint8_t*                                    path;
	const uint8_t*                                    target;
	const uint8_t*                                    name;
	size_t                                            name_size;

	if (!request_has_rights(call, CAP_CALL | CAP_WRITE)) return reply_transport(call, SYSCALL_STATUS_DENIED);
	if (!copy_request(call, data, &request, sizeof(request)) || request.reserved != 0u || request.path_size == 0u ||
	    request.path_size > CAP_MAX_REQUEST_SIZE - sizeof(request) ||
	    request.target_size > CAP_MAX_REQUEST_SIZE - sizeof(request) - request.path_size ||
	    sizeof(request) + request.path_size + request.target_size != call->request_size ||
	    call->response_capacity < sizeof(struct filesystem_response_header))
		return reply_transport(call, SYSCALL_STATUS_BAD_ARGUMENT);
	path   = (const uint8_t*)data + sizeof(request);
	target = path + request.path_size;
	if (!filesystem_relative_path_valid(path, request.path_size) ||
	    !filesystem_symbolic_link_target_valid(target, request.target_size))
		return reply_transport(call, SYSCALL_STATUS_BAD_ARGUMENT);
	status = walk(directory, path, request.path_size, true, &parent, &name, &name_size);
	if (status != FILESYSTEM_STATUS_OK) return reply_domain(call, status);
	if (find_child(parent, name, name_size) != NULL) return reply_domain(call, FILESYSTEM_STATUS_ALREADY_EXISTS);
	node = create_node(parent, FILESYSTEM_NODE_SYMBOLIC_LINK, name, name_size);
	if (node == NULL) return reply_domain(call, FILESYSTEM_STATUS_NO_SPACE);
	node->data = malloc(request.target_size);
	if (node->data == NULL) {
		unlink_node(node);
		release_node(node);
		return reply_domain(call, FILESYSTEM_STATUS_NO_SPACE);
	}
	memcpy(node->data, target, request.target_size);
	node->size = request.target_size;
	return reply_domain(call, FILESYSTEM_STATUS_OK);
}

static bool handle_directory_remove(const struct cap_request* call, const void* data, struct ramfs_node* directory) {
	struct ramfs_node*     node;
	enum filesystem_status status;
	const uint8_t*         path;
	size_t                 path_size;
	bool                   replied;

	path = path_request_path(call, data, CAP_WRITE, sizeof(struct filesystem_response_header), &path_size, &replied);
	if (path == NULL) return replied;
	status = walk(directory, path, path_size, false, &node, NULL, NULL);
	if (status != FILESYSTEM_STATUS_OK) return reply_domain(call, status);
	if (node->type == FILESYSTEM_NODE_DIRECTORY && node->child_count != 0u)
		return reply_domain(call, FILESYSTEM_STATUS_DIRECTORY_NOT_EMPTY);
	unlink_node(node);
	release_node(node);
	return reply_domain(call, FILESYSTEM_STATUS_OK);
}

static bool node_contains(const struct ramfs_node* ancestor, const struct ramfs_node* node) {
	for (; node != NULL; node = node->parent) {
		if (node == ancestor) return true;
	}
	return false;
}

static bool handle_directory_rename(const struct cap_request* call, const void* data, struct ramfs_node* directory) {
	struct filesystem_directory_rename_request request;
	struct ramfs_node*                         node;
	struct ramfs_node*                         parent;
	enum filesystem_status                     status;
	const uint8_t*                             old_path;
	const uint8_t*                             new_path;
	const uint8_t*                             name;
	size_t                                     name_size;
	uint8_t*                                   name_copy;

	if (!request_has_rights(call, CAP_CALL | CAP_WRITE)) return reply_transport(call, SYSCALL_STATUS_DENIED);
	if (!copy_request(call, data, &request, sizeof(request)) || request.reserved != 0u || request.old_path_size == 0u ||
	    request.new_path_size == 0u || request.old_path_size > CAP_MAX_REQUEST_SIZE - sizeof(request) ||
	    request.new_path_size > CAP_MAX_REQUEST_SIZE - sizeof(request) - request.old_path_size ||
	    sizeof(request) + request.old_path_size + request.new_path_size != call->request_size ||
	    call->response_capacity < sizeof(struct filesystem_response_header))
		return reply_transport(call, SYSCALL_STATUS_BAD_ARGUMENT);
	old_path = (const uint8_t*)data + sizeof(request);
	new_path = old_path + request.old_path_size;
	if (!filesystem_relative_path_valid(old_path, request.old_path_size) ||
	    !filesystem_relative_path_valid(new_path, request.new_path_size))
		return reply_transport(call, SYSCALL_STATUS_BAD_ARGUMENT);
	status = walk(directory, old_path, request.old_path_size, false, &node, NULL, NULL);
	if (status != FILESYSTEM_STATUS_OK) return reply_domain(call, status);
	status = walk(directory, new_path, request.new_path_size, true, &parent, &name, &name_size);
	if (status != FILESYSTEM_STATUS_OK) return reply_domain(call, status);
	if (find_child(parent, name, name_size) != NULL) return reply_domain(call, FILESYSTEM_STATUS_ALREADY_EXISTS);
	if (node_contains(node, parent)) return reply_domain(call, FILESYSTEM_STATUS_INVALID_ARGUMENT);
	name_copy = malloc(name_size);
	if (name_copy == NULL) return reply_domain(call, FILESYSTEM_STATUS_NO_SPACE);
	memcpy(name_copy, name, name_size);
	unlink_node(node);
	free(node->name);
	node->name      = name_copy;
	node->name_size = name_size;
	attach_node(parent, node);
	return reply_domain(call, FILESYSTEM_STATUS_OK);
}

static bool dispatch_directory(const struct cap_request* call, const void* data,
                               const struct filesystem_request_header* header, struct ramfs_node* directory) {
	switch (header->op) {
	case FILESYSTEM_DIRECTORY_OP_INFO:
		return handle_node_info(call, data, directory);
	case FILESYSTEM_DIRECTORY_OP_OPEN:
		return handle_directory_open(call, data, directory);
	case FILESYSTEM_DIRECTORY_OP_ENUMERATE:
		return handle_directory_enumerate(call, data, directory);
	case FILESYSTEM_DIRECTORY_OP_CREATE_FILE:
		return handle_directory_create(call, data, directory, FILESYSTEM_NODE_FILE);
	case FILESYSTEM_DIRECTORY_OP_CREATE_DIRECTORY:
		return handle_directory_create(call, data, directory, FILESYSTEM_NODE_DIRECTORY);
	case FILESYSTEM_DIRECTORY_OP_REMOVE:
		return handle_directory_remove(call, data, directory);
	case FILESYSTEM_DIRECTORY_OP_RENAME:
		return handle_directory_rename(call, data, directory);
	case FILESYSTEM_DIRECTORY_OP_LOOKUP:
		return handle_directory_lookup(call, data, directory);
	case FILESYSTEM_DIRECTORY_OP_CREATE_SYMBOLIC_LINK:
		return handle_directory_create_symbolic_link(call, data, directory);
	case FILESYSTEM_DIRECTORY_OP_READ_SYMBOLIC_LINK:
		return handle_directory_read_symbolic_link(call, data, directory);
	default:
		return reply_transport(call, SYSCALL_STATUS_BAD_ARGUMENT);
	}
}

static bool handle_file_read(const struct cap_request* call, const void* data, const struct ramfs_node* file) {
	struct filesystem_file_read_request   request;
	struct filesystem_file_read_response* response  = (void*)response_buffer;
	size_t                                read_size = 0u;

	if (!request_has_rights(call, CAP_CALL | CAP_READ)) return reply_transport(call, SYSCALL_STATUS_DENIED);
	if (!copy_request(call, data, &request, sizeof(request)) || call->request_size != sizeof(request) ||
	    request.reserved != 0u || request.size > CAP_MAX_RESPONSE_SIZE - sizeof(*response) ||
	    call->response_capacity < sizeof(*response) || request.size > call->response_capacity - sizeof(*response))
		return reply_transport(call, SYSCALL_STATUS_BAD_ARGUMENT);
	if (request.offset < file->size) {
		uint64_t available = file->size - request.offset;
		read_size          = available > request.size ? (size_t)request.size : (size_t)available;
	}
	response->header = (struct filesystem_response_header){.status = FILESYSTEM_STATUS_OK, .reserved = 0u};
	if (read_size != 0u) memcpy(response->data, file->data + request.offset, read_size);
	return reply_request(call->call_id, response, sizeof(*response) + read_size, SYSCALL_STATUS_OK);
}

static bool handle_file_write(const struct cap_request* call, const void* data, struct ramfs_node* file) {
	struct filesystem_file_write_request  request;
	struct filesystem_file_write_response response = {
		.header = {.status = FILESYSTEM_STATUS_OK, .reserved = 0u},
	};

	if (!request_has_rights(call, CAP_CALL | CAP_WRITE)) return reply_transport(call, SYSCALL_STATUS_DENIED);
	if (!copy_request(call, data, &request, sizeof(request)) || request.reserved != 0u ||
	    request.size > CAP_MAX_REQUEST_SIZE - sizeof(request) || sizeof(request) + request.size != call->request_size ||
	    call->response_capacity < sizeof(response))
		return reply_transport(call, SYSCALL_STATUS_BAD_ARGUMENT);
	if (request.offset > SIZE_MAX - request.size) return reply_domain(call, FILESYSTEM_STATUS_NO_SPACE);
	if (request.offset + request.size > file->size && !resize_node(file, request.offset + request.size))
		return reply_domain(call, FILESYSTEM_STATUS_NO_SPACE);
	if (request.size != 0u) memcpy(file->data + request.offset, (const uint8_t*)data + sizeof(request), request.size);
	response.size = request.size;
	return reply_request(call->call_id, &response, sizeof(response), SYSCALL_STATUS_OK);
}

static bool handle_file_resize(const struct cap_request* call, const void* data, struct ramfs_node* file) {
	struct filesystem_file_resize_request request;

	if (!request_has_rights(call, CAP_CALL | CAP_WRITE)) return reply_transport(call, SYSCALL_STATUS_DENIED);
	if (!copy_request(call, data, &request, sizeof(request)) || call->request_size != sizeof(request) ||
	    request.reserved != 0u || call->response_capacity < sizeof(struct filesystem_response_header))
		return reply_transport(call, SYSCALL_STATUS_BAD_ARGUMENT);
	if (!resize_node(file, request.size)) return reply_domain(call, FILESYSTEM_STATUS_NO_SPACE);
	return reply_domain(call, FILESYSTEM_STATUS_OK);
}

static bool dispatch_file(const struct cap_request* call, const void* data,
                          const struct filesystem_request_header* header, struct ramfs_node* file) {
	switch (header->op) {
	case FILESYSTEM_FILE_OP_INFO:
		return handle_node_info(call, data, file);
	case FILESYSTEM_FILE_OP_READ:
		return handle_file_read(call, data, file);
	case FILESYSTEM_FILE_OP_WRITE:
		return handle_file_write(call, data, file);
	case FILESYSTEM_FILE_OP_RESIZE:
		return handle_file_resize(call, data, file);
	default:
		return reply_transport(call, SYSCALL_STATUS_BAD_ARGUMENT);
	}
}

static bool dispatch_request(const struct cap_request* call, const void* data) {
	struct filesystem_request_header header;
	struct ramfs_handle*             handle;
	struct ramfs_node*               node;

	if (!request_has_rights(call, CAP_CALL)) return reply_transport(call, SYSCALL_STATUS_DENIED);
	if (!copy_request(call, data, &header, sizeof(header))) return reply_transport(call, SYSCALL_STATUS_BAD_ARGUMENT);
	if (call->object_id == RAMFS_ROOT_OBJECT_ID) {
		node = &root;
	}
	else {
		handle = find_handle(call->object_id);
		if (handle == NULL) return reply_transport(call, SYSCALL_STATUS_BAD_ARGUMENT);
		node = handle->node;
	}
	if (node->type == FILESYSTEM_NODE_DIRECTORY) return dispatch_directory(call, data, &header, node);
	return dispatch_file(call, data, &header, node);
}

static bool advertise_ramfs(void) {
	const struct init_service_selector selector = {
		.namespace_path = FILESYSTEM_NAMESPACE,
		.protocol       = FILESYSTEM_PROTOCOL_NAME,
		.major          = FILESYSTEM_PROTOCOL_VERSION_MAJOR,
		.service        = RAMFS_SERVICE_NAME,
	};
	struct init_call_result result;
	struct self_info        self;
	syscall_status_t        status;

	status = process_self_info(&self);
	if (status != SYSCALL_STATUS_OK || self.pid == PROCESS_PID_INVALID) return false;
	status = channel_create(&ramfs_endpoint, &ramfs_activity);
	if (status != SYSCALL_STATUS_OK) return false;
	status = cap_publish(ramfs_endpoint, RAMFS_ROOT_OBJECT_ID, self.pid, RAMFS_SOURCE_RIGHTS, &ramfs_root_cap);
	if (status != SYSCALL_STATUS_OK) return false;
	result = init_advertise(&selector, FILESYSTEM_PROTOCOL_VERSION_MINOR, ramfs_root_cap, RAMFS_CLIENT_RIGHTS);
	return result.status == INIT_REGISTRY_OK && result.transport_status == SYSCALL_STATUS_OK;
}

static void handle_channel_event(const struct channel_event* event) {
	struct ramfs_handle* handle;

	if (event->type != CHANNEL_EVENT_CAP_ZERO_GRANTS || event->object_id == RAMFS_ROOT_OBJECT_ID) return;
	handle = find_handle(event->object_id);
	if (handle == NULL || cap_unpublish_if_unused(ramfs_endpoint, event->object_id) != SYSCALL_STATUS_OK) return;
	unlink_handle(handle);
	release_handle(handle);
}

static int ramfs_run(void) {
	struct cap_request call;
	bool               received;
	syscall_status_t   status;

	if (!advertise_ramfs()) {
		printf("ramfs: advertisement failed\n");
		return 1;
	}
	printf("ramfs: advertised %s/%s@%u.%u/%s\n",
	       FILESYSTEM_NAMESPACE,
	       FILESYSTEM_PROTOCOL_NAME,
	       FILESYSTEM_PROTOCOL_VERSION_MAJOR,
	       FILESYSTEM_PROTOCOL_VERSION_MINOR,
	       RAMFS_SERVICE_NAME);
	for (;;) {
		do {
			received = false;
			status   = channel_recv(ramfs_endpoint, &call, request_buffer, sizeof(request_buffer), &received);
			if (status != SYSCALL_STATUS_OK) {
				printf("ramfs: channel receive failed: %u\n", (unsigned)status);
				return 1;
			}
			if (received && !dispatch_request(&call, request_buffer)) printf("ramfs: channel reply failed\n");
		} while (received);
		do {
			struct channel_event event;
			received = false;
			status   = channel_event_recv(ramfs_endpoint, &event, &received);
			if (status != SYSCALL_STATUS_OK) return 1;
			if (received) handle_channel_event(&event);
		} while (received);
		struct signal_message activity;
		status = signal_wait(ramfs_activity, &activity);
		if (status != SYSCALL_STATUS_OK) return 1;
	}
}

int main(int argc, char** argv, size_t capc, const cap_id_t* capv) {
	(void)argv;
	if (argc != 0 || capc != 0u || capv != NULL) return 1;
	return ramfs_run();
}
