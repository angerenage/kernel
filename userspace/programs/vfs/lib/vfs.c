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

static struct filesystem_call_result vfs_open_node(cap_id_t vfs_cap, uint32_t op, const void* path, size_t path_size,
                                                   cap_rights_t rights, uint32_t expected_type,
                                                   struct filesystem_node_handle* out_node) {
	struct vfs_open_request*        request;
	struct filesystem_open_response response;
	struct filesystem_call_result   call_result;
	size_t                          request_size;

	if (out_node == NULL) return local_error();
	*out_node = (struct filesystem_node_handle){.capability = CAP_ID_INVALID};
	if (vfs_cap == CAP_ID_INVALID || path_size > CAP_MAX_REQUEST_SIZE - sizeof(*request) ||
	    !vfs_absolute_path_valid(path, path_size) || !rights_valid(rights))
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
	call_result = call_fixed(vfs_cap, request, request_size, &response, sizeof(response));
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
	return vfs_open_node(vfs_cap, VFS_OP_OPEN, path, path_size, rights, FILESYSTEM_NODE_INVALID, out_node);
}

struct filesystem_call_result vfs_create_file(cap_id_t vfs_cap, const void* path, size_t path_size, cap_rights_t rights,
                                              struct filesystem_node_handle* out_node) {
	return vfs_open_node(vfs_cap, VFS_OP_CREATE_FILE, path, path_size, rights, FILESYSTEM_NODE_FILE, out_node);
}

struct filesystem_call_result vfs_create_directory(cap_id_t vfs_cap, const void* path, size_t path_size,
                                                   cap_rights_t rights, struct filesystem_node_handle* out_node) {
	return vfs_open_node(
		vfs_cap, VFS_OP_CREATE_DIRECTORY, path, path_size, rights, FILESYSTEM_NODE_DIRECTORY, out_node);
}

static struct filesystem_call_result vfs_path_call(cap_id_t vfs_cap, uint32_t op, const void* path, size_t path_size) {
	struct vfs_path_request*          request;
	struct filesystem_response_header response;
	struct filesystem_call_result     call_result;
	size_t                            request_size;

	if (vfs_cap == CAP_ID_INVALID || path_size > CAP_MAX_REQUEST_SIZE - sizeof(*request) ||
	    !vfs_absolute_path_valid(path, path_size))
		return local_error();
	request_size = sizeof(*request) + path_size;
	request      = malloc(request_size);
	if (request == NULL) return result(FILESYSTEM_STATUS_NONE, SYSCALL_STATUS_FAILED);
	*request = (struct vfs_path_request){
		.header    = {.op = op},
		.path_size = (uint32_t)path_size,
	};
	memcpy(request->path, path, path_size);
	call_result = call_fixed(vfs_cap, request, request_size, &response, sizeof(response));
	free(request);
	return call_result;
}

struct filesystem_call_result vfs_remove(cap_id_t vfs_cap, const void* path, size_t path_size) {
	return vfs_path_call(vfs_cap, VFS_OP_REMOVE, path, path_size);
}

struct filesystem_call_result vfs_rename(cap_id_t vfs_cap, const void* old_path, size_t old_path_size,
                                         const void* new_path, size_t new_path_size) {
	struct vfs_rename_request*        request;
	struct filesystem_response_header response;
	struct filesystem_call_result     call_result;
	size_t                            request_size;

	if (vfs_cap == CAP_ID_INVALID || old_path_size > CAP_MAX_REQUEST_SIZE - sizeof(*request) ||
	    new_path_size > CAP_MAX_REQUEST_SIZE - sizeof(*request) - old_path_size ||
	    !vfs_absolute_path_valid(old_path, old_path_size) || !vfs_absolute_path_valid(new_path, new_path_size))
		return local_error();
	request_size = sizeof(*request) + old_path_size + new_path_size;
	request      = malloc(request_size);
	if (request == NULL) return result(FILESYSTEM_STATUS_NONE, SYSCALL_STATUS_FAILED);
	*request = (struct vfs_rename_request){
		.header        = {.op = VFS_OP_RENAME},
		.old_path_size = (uint32_t)old_path_size,
		.new_path_size = (uint32_t)new_path_size,
		.reserved      = 0u,
	};
	memcpy(request->paths, old_path, old_path_size);
	memcpy(request->paths + old_path_size, new_path, new_path_size);
	call_result = call_fixed(vfs_cap, request, request_size, &response, sizeof(response));
	free(request);
	return call_result;
}

struct filesystem_call_result vfs_directory_info(cap_id_t directory_cap, struct filesystem_node_info* out_info) {
	return filesystem_directory_info(directory_cap, out_info);
}

static bool directory_entry_valid(const struct filesystem_directory_entry* entry) {
	return entry->name_size <= FILESYSTEM_NAME_MAX && entry->reserved == 0u && node_info_valid(&entry->info) &&
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
