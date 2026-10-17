#include <libc/stdlib.h>
#include <libc/string.h>
#include <protocol/vfs.h>
#include <runtime/vfs.h>
#include <stdbool.h>
#include <system/capability.h>

static struct filesystem_call_result result(enum filesystem_status status, syscall_status_t transport_status) {
	return (struct filesystem_call_result){.transport_status = transport_status, .status = status};
}

static struct filesystem_call_result local_error(void) {
	return result(FILESYSTEM_STATUS_INVALID_ARGUMENT, SYSCALL_STATUS_BAD_ARGUMENT);
}

static struct filesystem_call_result failed_response(void) {
	return result(FILESYSTEM_STATUS_NONE, SYSCALL_STATUS_FAILED);
}

static bool response_header_valid(const struct filesystem_response_header* header) {
	return header->reserved == 0u && header->status < FILESYSTEM_STATUS_COUNT;
}

static bool node_info_valid(const struct filesystem_node_info* info) {
	return info->type == FILESYSTEM_NODE_FILE || info->type == FILESYSTEM_NODE_DIRECTORY;
}

static bool entry_info_valid(const struct filesystem_node_info* info) {
	return node_info_valid(info) || info->type == FILESYSTEM_NODE_SYMBOLIC_LINK;
}

static bool path_valid(const void* path, size_t path_size, bool absolute) {
	return absolute ? vfs_absolute_path_valid(path, path_size) : filesystem_relative_path_valid(path, path_size);
}

static bool rights_valid(cap_rights_t rights) {
	return (rights & ~FILESYSTEM_NODE_REQUEST_RIGHTS) == 0u;
}

bool vfs_absolute_path_valid(const void* path, size_t path_size) {
	const uint8_t* bytes = path;

	if (path == NULL || path_size == 0u || bytes[0] != '/') return false;
	if (path_size == 1u) return true;
	return filesystem_relative_path_valid(bytes + 1u, path_size - 1u);
}

static struct filesystem_call_result call_fixed(cap_id_t cap, const void* request, size_t request_size, void* response,
                                                size_t success_size) {
	struct filesystem_response_header* header        = response;
	size_t                             response_size = 0u;
	syscall_status_t                   status;

	status = cap_call(cap, request, request_size, response, success_size, &response_size);
	if (status != SYSCALL_STATUS_OK) return result(FILESYSTEM_STATUS_NONE, status);
	if (response_size < sizeof(*header) || !response_header_valid(header)) return failed_response();
	if (header->status != FILESYSTEM_STATUS_OK) {
		if (response_size != sizeof(*header)) return failed_response();
		return result((enum filesystem_status)header->status, SYSCALL_STATUS_OK);
	}
	if (response_size != success_size) return failed_response();
	return result(FILESYSTEM_STATUS_OK, SYSCALL_STATUS_OK);
}

static struct filesystem_call_result open_node(cap_id_t cap, uint32_t op, const void* path, size_t path_size,
                                               cap_rights_t rights, uint32_t expected_type, bool absolute,
                                               struct filesystem_node_handle* out_node) {
	struct vfs_open_request*        request;
	struct filesystem_open_response response;
	struct filesystem_call_result   call_result;
	size_t                          request_size;

	if (out_node == NULL) return local_error();
	*out_node = (struct filesystem_node_handle){.capability = CAP_ID_INVALID};
	if (cap == CAP_ID_INVALID || path_size > CAP_MAX_REQUEST_SIZE - sizeof(*request) || !rights_valid(rights) ||
	    (absolute ? !vfs_absolute_path_valid(path, path_size) : !filesystem_relative_path_valid(path, path_size)))
		return local_error();
	request_size = sizeof(*request) + path_size;
	request      = malloc(request_size);
	if (request == NULL) return result(FILESYSTEM_STATUS_NONE, SYSCALL_STATUS_FAILED);
	*request = (struct vfs_open_request){
		.header    = {.op = op},
		.path_size = (uint32_t)path_size,
		.rights    = rights,
	};
	memcpy(request->path, path, path_size);
	call_result = call_fixed(cap, request, request_size, &response, sizeof(response));
	free(request);
	if (call_result.transport_status != SYSCALL_STATUS_OK || call_result.status != FILESYSTEM_STATUS_OK)
		return call_result;
	if (!node_info_valid(&response.info) ||
	    (expected_type != FILESYSTEM_NODE_INVALID && response.info.type != expected_type) ||
	    response.capability == CAP_ID_INVALID)
		return failed_response();
	*out_node = (struct filesystem_node_handle){.info = response.info, .capability = response.capability};
	return call_result;
}

struct filesystem_call_result vfs_open(cap_id_t vfs_cap, const void* path, size_t path_size, cap_rights_t rights,
                                       struct filesystem_node_handle* out_node) {
	return open_node(vfs_cap, VFS_OP_OPEN, path, path_size, rights, FILESYSTEM_NODE_INVALID, true, out_node);
}

struct filesystem_call_result vfs_create_file(cap_id_t vfs_cap, const void* path, size_t path_size, cap_rights_t rights,
                                              struct filesystem_node_handle* out_node) {
	return open_node(vfs_cap, VFS_OP_CREATE_FILE, path, path_size, rights, FILESYSTEM_NODE_FILE, true, out_node);
}

struct filesystem_call_result vfs_create_directory(cap_id_t vfs_cap, const void* path, size_t path_size,
                                                   cap_rights_t rights, struct filesystem_node_handle* out_node) {
	return open_node(
		vfs_cap, VFS_OP_CREATE_DIRECTORY, path, path_size, rights, FILESYSTEM_NODE_DIRECTORY, true, out_node);
}

static struct filesystem_call_result path_call(cap_id_t cap, uint32_t op, const void* path, size_t path_size,
                                               bool absolute) {
	struct vfs_path_request*          request;
	struct filesystem_response_header response;
	struct filesystem_call_result     call_result;
	size_t                            request_size;

	if (cap == CAP_ID_INVALID || path_size > CAP_MAX_REQUEST_SIZE - sizeof(*request) ||
	    (absolute ? !vfs_absolute_path_valid(path, path_size) : !filesystem_relative_path_valid(path, path_size)))
		return local_error();
	request_size = sizeof(*request) + path_size;
	request      = malloc(request_size);
	if (request == NULL) return result(FILESYSTEM_STATUS_NONE, SYSCALL_STATUS_FAILED);
	*request = (struct vfs_path_request){
		.header    = {.op = op},
		.path_size = (uint32_t)path_size,
	};
	memcpy(request->path, path, path_size);
	call_result = call_fixed(cap, request, request_size, &response, sizeof(response));
	free(request);
	return call_result;
}

struct filesystem_call_result vfs_remove(cap_id_t vfs_cap, const void* path, size_t path_size) {
	return path_call(vfs_cap, VFS_OP_REMOVE, path, path_size, true);
}

static struct filesystem_call_result rename_path(cap_id_t cap, uint32_t op, const void* old_path, size_t old_path_size,
                                                 const void* new_path, size_t new_path_size, bool absolute) {
	struct vfs_rename_request*        request;
	struct filesystem_response_header response;
	struct filesystem_call_result     call_result;
	size_t                            request_size;

	if (cap == CAP_ID_INVALID || old_path_size > CAP_MAX_REQUEST_SIZE - sizeof(*request) ||
	    new_path_size > CAP_MAX_REQUEST_SIZE - sizeof(*request) - old_path_size ||
	    (absolute
			 ? (!vfs_absolute_path_valid(old_path, old_path_size) || !vfs_absolute_path_valid(new_path, new_path_size))
			 : (!filesystem_relative_path_valid(old_path, old_path_size) ||
			    !filesystem_relative_path_valid(new_path, new_path_size))))
		return local_error();
	request_size = sizeof(*request) + old_path_size + new_path_size;
	request      = malloc(request_size);
	if (request == NULL) return result(FILESYSTEM_STATUS_NONE, SYSCALL_STATUS_FAILED);
	*request = (struct vfs_rename_request){
		.header        = {.op = op},
		.old_path_size = (uint32_t)old_path_size,
		.new_path_size = (uint32_t)new_path_size,
		.reserved      = 0u,
	};
	memcpy(request->paths, old_path, old_path_size);
	memcpy(request->paths + old_path_size, new_path, new_path_size);
	call_result = call_fixed(cap, request, request_size, &response, sizeof(response));
	free(request);
	return call_result;
}

struct filesystem_call_result vfs_rename(cap_id_t vfs_cap, const void* old_path, size_t old_path_size,
                                         const void* new_path, size_t new_path_size) {
	return rename_path(vfs_cap, VFS_OP_RENAME, old_path, old_path_size, new_path, new_path_size, true);
}

struct filesystem_call_result vfs_directory_info(cap_id_t directory_cap, struct filesystem_node_info* out_info) {
	return filesystem_directory_info(directory_cap, out_info);
}

struct filesystem_call_result vfs_directory_open(cap_id_t directory_cap, const void* path, size_t path_size,
                                                 cap_rights_t rights, struct filesystem_node_handle* out_node) {
	return open_node(
		directory_cap, VFS_DIRECTORY_OP_OPEN, path, path_size, rights, FILESYSTEM_NODE_INVALID, false, out_node);
}

struct filesystem_call_result vfs_directory_create_file(cap_id_t directory_cap, const void* path, size_t path_size,
                                                        cap_rights_t rights, struct filesystem_node_handle* out_node) {
	return open_node(
		directory_cap, VFS_DIRECTORY_OP_CREATE_FILE, path, path_size, rights, FILESYSTEM_NODE_FILE, false, out_node);
}

struct filesystem_call_result vfs_directory_create_directory(cap_id_t directory_cap, const void* path, size_t path_size,
                                                             cap_rights_t                   rights,
                                                             struct filesystem_node_handle* out_node) {
	return open_node(directory_cap,
	                 VFS_DIRECTORY_OP_CREATE_DIRECTORY,
	                 path,
	                 path_size,
	                 rights,
	                 FILESYSTEM_NODE_DIRECTORY,
	                 false,
	                 out_node);
}

static bool directory_entry_valid(const struct filesystem_directory_entry* entry) {
	return entry->name_size <= FILESYSTEM_NAME_MAX && entry->reserved == 0u && entry_info_valid(&entry->info) &&
	       filesystem_relative_path_valid(entry->name, entry->name_size) &&
	       memchr(entry->name, '/', entry->name_size) == NULL;
}

struct filesystem_call_result vfs_directory_enumerate(cap_id_t directory_cap, uint64_t offset,
                                                      struct filesystem_directory_entry* entries, size_t count,
                                                      size_t* out_returned, uint64_t* out_total) {
	const size_t maximum_count =
		(CAP_MAX_RESPONSE_SIZE - sizeof(struct filesystem_directory_enumerate_response)) / sizeof(*entries);
	struct filesystem_directory_enumerate_request request = {
		.header   = {.op = VFS_DIRECTORY_OP_ENUMERATE},
		.reserved = 0u,
		.offset   = offset,
	};
	struct filesystem_directory_enumerate_response* response;
	struct filesystem_call_result                   call_result;
	size_t                                          capacity;
	size_t                                          response_size = 0u;
	size_t                                          transfer_count;
	syscall_status_t                                status;

	if (out_returned == NULL || out_total == NULL) return local_error();
	*out_returned = 0u;
	*out_total    = 0u;
	if (directory_cap == CAP_ID_INVALID || (count != 0u && entries == NULL)) return local_error();
	transfer_count = count > maximum_count ? maximum_count : count;
	request.count  = transfer_count;
	capacity       = sizeof(*response) + transfer_count * sizeof(*entries);
	response       = malloc(capacity);
	if (response == NULL) return result(FILESYSTEM_STATUS_NONE, SYSCALL_STATUS_FAILED);
	status = cap_call(directory_cap, &request, sizeof(request), response, capacity, &response_size);
	if (status != SYSCALL_STATUS_OK) {
		free(response);
		return result(FILESYSTEM_STATUS_NONE, status);
	}
	if (response_size < sizeof(response->header) || !response_header_valid(&response->header)) {
		free(response);
		return failed_response();
	}
	call_result = result((enum filesystem_status)response->header.status, SYSCALL_STATUS_OK);
	if (response->header.status != FILESYSTEM_STATUS_OK) {
		if (response_size != sizeof(response->header)) call_result = failed_response();
		free(response);
		return call_result;
	}
	if (response_size < sizeof(*response) || response->returned > transfer_count ||
	    response->returned > response->total ||
	    (response->returned != 0u && (offset > response->total || response->returned > response->total - offset)) ||
	    response_size != sizeof(*response) + (size_t)response->returned * sizeof(*entries)) {
		free(response);
		return failed_response();
	}
	for (size_t i = 0u; i < (size_t)response->returned; i++) {
		if (!directory_entry_valid(&response->entries[i])) {
			free(response);
			return failed_response();
		}
	}
	if (response->returned != 0u) memcpy(entries, response->entries, (size_t)response->returned * sizeof(*entries));
	*out_returned = (size_t)response->returned;
	*out_total    = response->total;
	free(response);
	return call_result;
}

struct filesystem_call_result vfs_directory_remove(cap_id_t directory_cap, const void* path, size_t path_size) {
	return path_call(directory_cap, VFS_DIRECTORY_OP_REMOVE, path, path_size, false);
}

struct filesystem_call_result vfs_directory_rename(cap_id_t directory_cap, const void* old_path, size_t old_path_size,
                                                   const void* new_path, size_t new_path_size) {
	return rename_path(directory_cap, VFS_DIRECTORY_OP_RENAME, old_path, old_path_size, new_path, new_path_size, false);
}

static struct vfs_path_request* path_request(uint32_t op, const void* path, size_t path_size, bool absolute,
                                             size_t* out_size) {
	struct vfs_path_request* request;

	if (path_size > CAP_MAX_REQUEST_SIZE - sizeof(*request) || !path_valid(path, path_size, absolute)) return NULL;
	*out_size = sizeof(*request) + path_size;
	request   = malloc(*out_size);
	if (request == NULL) return NULL;
	*request = (struct vfs_path_request){
		.header    = {.op = op},
		.path_size = (uint32_t)path_size,
	};
	memcpy(request->path, path, path_size);
	return request;
}

/* Issue a request whose success response is a header followed by up to capacity bytes. */
static struct filesystem_call_result call_variable(cap_id_t cap, const void* request, size_t request_size, void* data,
                                                   size_t capacity, size_t* out_size) {
	struct filesystem_response_header* response;
	struct filesystem_call_result      call_result;
	size_t                             response_size = 0u;
	syscall_status_t                   status;

	response = malloc(sizeof(*response) + capacity);
	if (response == NULL) return result(FILESYSTEM_STATUS_NONE, SYSCALL_STATUS_FAILED);
	status = cap_call(cap, request, request_size, response, sizeof(*response) + capacity, &response_size);
	if (status != SYSCALL_STATUS_OK) {
		free(response);
		return result(FILESYSTEM_STATUS_NONE, status);
	}
	if (response_size < sizeof(*response) || response_size > sizeof(*response) + capacity ||
	    !response_header_valid(response)) {
		free(response);
		return failed_response();
	}
	call_result = result((enum filesystem_status)response->status, SYSCALL_STATUS_OK);
	if (response->status != FILESYSTEM_STATUS_OK) {
		if (response_size != sizeof(*response)) call_result = failed_response();
		free(response);
		return call_result;
	}
	*out_size = response_size - sizeof(*response);
	memcpy(data, response + 1, *out_size);
	free(response);
	return call_result;
}

static struct filesystem_call_result lookup_node(cap_id_t cap, uint32_t op, const void* path, size_t path_size,
                                                 bool absolute, struct filesystem_node_info* out_info) {
	struct vfs_path_request*        request;
	struct filesystem_info_response response;
	struct filesystem_call_result   call_result;
	size_t                          request_size;

	if (out_info == NULL) return local_error();
	*out_info = (struct filesystem_node_info){0};
	if (cap == CAP_ID_INVALID) return local_error();
	request = path_request(op, path, path_size, absolute, &request_size);
	if (request == NULL) return local_error();
	call_result = call_fixed(cap, request, request_size, &response, sizeof(response));
	free(request);
	if (call_result.transport_status != SYSCALL_STATUS_OK || call_result.status != FILESYSTEM_STATUS_OK)
		return call_result;
	if (!entry_info_valid(&response.info)) return failed_response();
	*out_info = response.info;
	return call_result;
}

static struct filesystem_call_result create_symbolic_link(cap_id_t cap, uint32_t op, const void* path, size_t path_size,
                                                          const void* target, size_t target_size, bool absolute) {
	struct vfs_symbolic_link_request* request;
	struct filesystem_response_header response;
	struct filesystem_call_result     call_result;
	size_t                            request_size;

	if (cap == CAP_ID_INVALID || path_size > CAP_MAX_REQUEST_SIZE - sizeof(*request) ||
	    target_size > CAP_MAX_REQUEST_SIZE - sizeof(*request) - path_size || !path_valid(path, path_size, absolute) ||
	    !filesystem_symbolic_link_target_valid(target, target_size))
		return local_error();
	request_size = sizeof(*request) + path_size + target_size;
	request      = malloc(request_size);
	if (request == NULL) return result(FILESYSTEM_STATUS_NONE, SYSCALL_STATUS_FAILED);
	*request = (struct vfs_symbolic_link_request){
		.header      = {.op = op},
		.path_size   = (uint32_t)path_size,
		.target_size = (uint32_t)target_size,
		.reserved    = 0u,
	};
	memcpy(request->data, path, path_size);
	memcpy(request->data + path_size, target, target_size);
	call_result = call_fixed(cap, request, request_size, &response, sizeof(response));
	free(request);
	return call_result;
}

static struct filesystem_call_result read_symbolic_link(cap_id_t cap, uint32_t op, const void* path, size_t path_size,
                                                        bool absolute, void* target, size_t capacity,
                                                        size_t* out_size) {
	struct vfs_path_request*      request;
	struct filesystem_call_result call_result;
	size_t                        request_size;
	size_t                        size = 0u;

	if (out_size == NULL) return local_error();
	*out_size = 0u;
	if (cap == CAP_ID_INVALID || target == NULL || capacity < FILESYSTEM_SYMBOLIC_LINK_MAX) return local_error();
	request = path_request(op, path, path_size, absolute, &request_size);
	if (request == NULL) return local_error();
	call_result = call_variable(cap, request, request_size, target, FILESYSTEM_SYMBOLIC_LINK_MAX, &size);
	free(request);
	if (call_result.transport_status != SYSCALL_STATUS_OK || call_result.status != FILESYSTEM_STATUS_OK)
		return call_result;
	if (!filesystem_symbolic_link_target_valid(target, size)) return failed_response();
	*out_size = size;
	return call_result;
}

struct filesystem_call_result vfs_lookup(cap_id_t vfs_cap, const void* path, size_t path_size,
                                         struct filesystem_node_info* out_info) {
	return lookup_node(vfs_cap, VFS_OP_LOOKUP, path, path_size, true, out_info);
}

struct filesystem_call_result vfs_create_symbolic_link(cap_id_t vfs_cap, const void* path, size_t path_size,
                                                       const void* target, size_t target_size) {
	return create_symbolic_link(vfs_cap, VFS_OP_CREATE_SYMBOLIC_LINK, path, path_size, target, target_size, true);
}

struct filesystem_call_result vfs_read_symbolic_link(cap_id_t vfs_cap, const void* path, size_t path_size, void* target,
                                                     size_t capacity, size_t* out_size) {
	return read_symbolic_link(vfs_cap, VFS_OP_READ_SYMBOLIC_LINK, path, path_size, true, target, capacity, out_size);
}

struct filesystem_call_result vfs_resolve(cap_id_t vfs_cap, const void* path, size_t path_size, char* resolved,
                                          size_t capacity, size_t* out_size) {
	struct vfs_path_request*      request;
	struct filesystem_call_result call_result;
	size_t                        request_size;
	size_t                        size = 0u;

	if (out_size == NULL) return local_error();
	*out_size = 0u;
	if (vfs_cap == CAP_ID_INVALID || resolved == NULL || capacity < VFS_PATH_MAX) return local_error();
	request = path_request(VFS_OP_RESOLVE, path, path_size, true, &request_size);
	if (request == NULL) return local_error();
	call_result = call_variable(vfs_cap, request, request_size, resolved, VFS_PATH_MAX, &size);
	free(request);
	if (call_result.transport_status != SYSCALL_STATUS_OK || call_result.status != FILESYSTEM_STATUS_OK)
		return call_result;
	if (!vfs_absolute_path_valid(resolved, size)) return failed_response();
	*out_size = size;
	return call_result;
}

struct filesystem_call_result vfs_directory_lookup(cap_id_t directory_cap, const void* path, size_t path_size,
                                                   struct filesystem_node_info* out_info) {
	return lookup_node(directory_cap, VFS_DIRECTORY_OP_LOOKUP, path, path_size, false, out_info);
}

struct filesystem_call_result vfs_directory_create_symbolic_link(cap_id_t directory_cap, const void* path,
                                                                 size_t path_size, const void* target,
                                                                 size_t target_size) {
	return create_symbolic_link(
		directory_cap, VFS_DIRECTORY_OP_CREATE_SYMBOLIC_LINK, path, path_size, target, target_size, false);
}

struct filesystem_call_result vfs_directory_read_symbolic_link(cap_id_t directory_cap, const void* path,
                                                               size_t path_size, void* target, size_t capacity,
                                                               size_t* out_size) {
	return read_symbolic_link(
		directory_cap, VFS_DIRECTORY_OP_READ_SYMBOLIC_LINK, path, path_size, false, target, capacity, out_size);
}

struct filesystem_call_result vfs_directory_mount(cap_id_t directory_cap, cap_id_t fs_root_cap, uint32_t flags) {
	const struct vfs_directory_mount_request request = {
		.header          = {.op = VFS_DIRECTORY_OP_MOUNT},
		.flags           = flags,
		.root_capability = fs_root_cap,
	};
	struct filesystem_response_header response;

	if (directory_cap == CAP_ID_INVALID || fs_root_cap == CAP_ID_INVALID || (flags & ~VFS_MOUNT_READ_ONLY) != 0u)
		return local_error();
	return call_fixed(directory_cap, &request, sizeof(request), &response, sizeof(response));
}

struct filesystem_call_result vfs_directory_unmount(cap_id_t directory_cap) {
	const struct filesystem_info_request request = {
		.header   = {.op = VFS_DIRECTORY_OP_UNMOUNT},
		.reserved = 0u,
	};
	struct filesystem_response_header response;

	if (directory_cap == CAP_ID_INVALID) return local_error();
	return call_fixed(directory_cap, &request, sizeof(request), &response, sizeof(response));
}
