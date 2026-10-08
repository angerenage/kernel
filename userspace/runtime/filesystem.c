#include <libc/stdlib.h>
#include <libc/string.h>
#include <runtime/filesystem.h>
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

static bool status_valid(uint32_t status) {
	return status < FILESYSTEM_STATUS_COUNT;
}

static bool node_info_valid(const struct filesystem_node_info* info) {
	return info->type == FILESYSTEM_NODE_FILE || info->type == FILESYSTEM_NODE_DIRECTORY;
}

static bool rights_valid(cap_rights_t rights) {
	return (rights & ~FILESYSTEM_NODE_REQUEST_RIGHTS) == 0u;
}

static bool component_valid(const uint8_t* component, size_t size) {
	if (size == 0u || size > FILESYSTEM_NAME_MAX) return false;
	if (size == 1u && component[0] == '.') return false;
	if (size == 2u && component[0] == '.' && component[1] == '.') return false;
	for (size_t i = 0u; i < size; i++) {
		if (component[i] == '/') return false;
	}
	return true;
}

bool filesystem_relative_path_valid(const void* path, size_t path_size) {
	const uint8_t* bytes           = path;
	size_t         component_start = 0u;

	if (path == NULL || path_size == 0u || bytes[0] == '/' || bytes[path_size - 1u] == '/') return false;
	for (size_t i = 0u; i < path_size; i++) {
		if (bytes[i] != '/') continue;
		if (!component_valid(bytes + component_start, i - component_start)) return false;
		component_start = i + 1u;
	}
	return component_valid(bytes + component_start, path_size - component_start);
}

static bool response_header_valid(const struct filesystem_response_header* header) {
	return header->reserved == 0u && status_valid(header->status);
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

static struct filesystem_call_result node_info(cap_id_t cap, uint32_t op, uint32_t expected_type,
                                               struct filesystem_node_info* out_info) {
	struct filesystem_info_request request = {
		.header   = {.op = op},
		.reserved = 0u,
	};
	struct filesystem_info_response response;
	struct filesystem_call_result   call_result;

	if (out_info == NULL) return local_error();
	*out_info = (struct filesystem_node_info){0};
	if (cap == CAP_ID_INVALID) return local_error();
	call_result = call_fixed(cap, &request, sizeof(request), &response, sizeof(response));
	if (call_result.transport_status != SYSCALL_STATUS_OK || call_result.status != FILESYSTEM_STATUS_OK)
		return call_result;
	if (!node_info_valid(&response.info) || response.info.type != expected_type) return failed_response();
	*out_info = response.info;
	return call_result;
}

struct filesystem_call_result filesystem_file_info(cap_id_t file_cap, struct filesystem_node_info* out_info) {
	return node_info(file_cap, FILESYSTEM_FILE_OP_INFO, FILESYSTEM_NODE_FILE, out_info);
}

struct filesystem_call_result filesystem_directory_info(cap_id_t directory_cap, struct filesystem_node_info* out_info) {
	return node_info(directory_cap, FILESYSTEM_DIRECTORY_OP_INFO, FILESYSTEM_NODE_DIRECTORY, out_info);
}

struct filesystem_call_result filesystem_file_read(cap_id_t file_cap, uint64_t offset, void* buffer, size_t size,
                                                   size_t* out_read) {
	const size_t maximum_size                   = CAP_MAX_RESPONSE_SIZE - sizeof(struct filesystem_file_read_response);
	struct filesystem_file_read_request request = {
		.header   = {.op = FILESYSTEM_FILE_OP_READ},
		.reserved = 0u,
		.offset   = offset,
	};
	struct filesystem_file_read_response* response;
	struct filesystem_call_result         call_result;
	size_t                                capacity;
	size_t                                response_size = 0u;
	size_t                                transfer_size;
	syscall_status_t                      status;

	if (out_read == NULL) return local_error();
	*out_read = 0u;
	if (file_cap == CAP_ID_INVALID || (size != 0u && buffer == NULL)) return local_error();
	transfer_size = size > maximum_size ? maximum_size : size;
	request.size  = transfer_size;
	capacity      = sizeof(*response) + transfer_size;
	response      = malloc(capacity);
	if (response == NULL) return result(FILESYSTEM_STATUS_NONE, SYSCALL_STATUS_FAILED);
	status = cap_call(file_cap, &request, sizeof(request), response, capacity, &response_size);
	if (status != SYSCALL_STATUS_OK) {
		free(response);
		return result(FILESYSTEM_STATUS_NONE, status);
	}
	if (response_size < sizeof(*response) || !response_header_valid(&response->header)) {
		free(response);
		return failed_response();
	}
	call_result = result((enum filesystem_status)response->header.status, SYSCALL_STATUS_OK);
	if (response->header.status != FILESYSTEM_STATUS_OK) {
		if (response_size != sizeof(*response)) call_result = failed_response();
		free(response);
		return call_result;
	}
	if (response_size > capacity) {
		free(response);
		return failed_response();
	}
	*out_read = response_size - sizeof(*response);
	if (*out_read != 0u) memcpy(buffer, response->data, *out_read);
	free(response);
	return call_result;
}

struct filesystem_call_result filesystem_file_write(cap_id_t file_cap, uint64_t offset, const void* data, size_t size,
                                                    size_t* out_written) {
	const size_t maximum_size = CAP_MAX_REQUEST_SIZE - sizeof(struct filesystem_file_write_request);
	struct filesystem_file_write_request  request_header;
	struct filesystem_file_write_request* request = &request_header;
	struct filesystem_file_write_response response;
	struct filesystem_call_result         call_result;
	size_t                                request_size;
	size_t                                transfer_size;

	if (out_written == NULL) return local_error();
	*out_written = 0u;
	if (file_cap == CAP_ID_INVALID || (size != 0u && data == NULL)) return local_error();
	transfer_size = size > maximum_size ? maximum_size : size;
	request_size  = sizeof(*request) + transfer_size;
	if (transfer_size != 0u) {
		request = malloc(request_size);
		if (request == NULL) return result(FILESYSTEM_STATUS_NONE, SYSCALL_STATUS_FAILED);
	}
	*request = (struct filesystem_file_write_request){
		.header   = {.op = FILESYSTEM_FILE_OP_WRITE},
		.reserved = 0u,
		.offset   = offset,
		.size     = transfer_size,
	};
	if (transfer_size != 0u) memcpy(request->data, data, transfer_size);
	call_result = call_fixed(file_cap, request, request_size, &response, sizeof(response));
	if (transfer_size != 0u) free(request);
	if (call_result.transport_status != SYSCALL_STATUS_OK || call_result.status != FILESYSTEM_STATUS_OK)
		return call_result;
	if (response.size > transfer_size || (transfer_size != 0u && response.size == 0u)) return failed_response();
	*out_written = (size_t)response.size;
	return call_result;
}

struct filesystem_call_result filesystem_file_resize(cap_id_t file_cap, uint64_t size) {
	const struct filesystem_file_resize_request request = {
		.header   = {.op = FILESYSTEM_FILE_OP_RESIZE},
		.reserved = 0u,
		.size     = size,
	};
	struct filesystem_response_header response;

	if (file_cap == CAP_ID_INVALID) return local_error();
	return call_fixed(file_cap, &request, sizeof(request), &response, sizeof(response));
}

static struct filesystem_call_result directory_open(cap_id_t directory_cap, uint32_t op, const void* path,
                                                    size_t path_size, cap_rights_t rights, uint32_t expected_type,
                                                    struct filesystem_node_handle* out_node) {
	struct filesystem_directory_open_request* request;
	struct filesystem_open_response           response;
	struct filesystem_call_result             call_result;
	size_t                                    request_size;

	if (out_node == NULL) return local_error();
	*out_node = (struct filesystem_node_handle){.capability = CAP_ID_INVALID};
	if (directory_cap == CAP_ID_INVALID || path_size > CAP_MAX_REQUEST_SIZE - sizeof(*request) ||
	    !filesystem_relative_path_valid(path, path_size) || !rights_valid(rights))
		return local_error();
	request_size = sizeof(*request) + path_size;
	request      = malloc(request_size);
	if (request == NULL) return result(FILESYSTEM_STATUS_NONE, SYSCALL_STATUS_FAILED);
	*request = (struct filesystem_directory_open_request){
		.header    = {.op = op},
		.path_size = (uint32_t)path_size,
		.rights    = rights,
	};
	memcpy(request->path, path, path_size);
	call_result = call_fixed(directory_cap, request, request_size, &response, sizeof(response));
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

struct filesystem_call_result filesystem_directory_open(cap_id_t directory_cap, const void* path, size_t path_size,
                                                        cap_rights_t rights, struct filesystem_node_handle* out_node) {
	return directory_open(
		directory_cap, FILESYSTEM_DIRECTORY_OP_OPEN, path, path_size, rights, FILESYSTEM_NODE_INVALID, out_node);
}

struct filesystem_call_result filesystem_directory_create_file(cap_id_t directory_cap, const void* path,
                                                               size_t path_size, cap_rights_t rights,
                                                               struct filesystem_node_handle* out_node) {
	return directory_open(
		directory_cap, FILESYSTEM_DIRECTORY_OP_CREATE_FILE, path, path_size, rights, FILESYSTEM_NODE_FILE, out_node);
}

struct filesystem_call_result filesystem_directory_create_directory(cap_id_t directory_cap, const void* path,
                                                                    size_t path_size, cap_rights_t rights,
                                                                    struct filesystem_node_handle* out_node) {
	return directory_open(directory_cap,
	                      FILESYSTEM_DIRECTORY_OP_CREATE_DIRECTORY,
	                      path,
	                      path_size,
	                      rights,
	                      FILESYSTEM_NODE_DIRECTORY,
	                      out_node);
}

static bool directory_entry_valid(const struct filesystem_directory_entry* entry) {
	return entry->reserved == 0u && node_info_valid(&entry->info) && component_valid(entry->name, entry->name_size);
}

struct filesystem_call_result filesystem_directory_enumerate(cap_id_t directory_cap, uint64_t offset,
                                                             struct filesystem_directory_entry* entries, size_t count,
                                                             size_t* out_returned, uint64_t* out_total) {
	const size_t maximum_count =
		(CAP_MAX_RESPONSE_SIZE - sizeof(struct filesystem_directory_enumerate_response)) / sizeof(*entries);
	struct filesystem_directory_enumerate_request request = {
		.header   = {.op = FILESYSTEM_DIRECTORY_OP_ENUMERATE},
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

static struct filesystem_call_result directory_path_call(cap_id_t directory_cap, uint32_t op, const void* path,
                                                         size_t path_size) {
	struct filesystem_directory_path_request* request;
	struct filesystem_response_header         response;
	struct filesystem_call_result             call_result;
	size_t                                    request_size;

	if (directory_cap == CAP_ID_INVALID || path_size > CAP_MAX_REQUEST_SIZE - sizeof(*request) ||
	    !filesystem_relative_path_valid(path, path_size))
		return local_error();
	request_size = sizeof(*request) + path_size;
	request      = malloc(request_size);
	if (request == NULL) return result(FILESYSTEM_STATUS_NONE, SYSCALL_STATUS_FAILED);
	*request = (struct filesystem_directory_path_request){
		.header    = {.op = op},
		.path_size = (uint32_t)path_size,
	};
	memcpy(request->path, path, path_size);
	call_result = call_fixed(directory_cap, request, request_size, &response, sizeof(response));
	free(request);
	return call_result;
}

struct filesystem_call_result filesystem_directory_remove(cap_id_t directory_cap, const void* path, size_t path_size) {
	return directory_path_call(directory_cap, FILESYSTEM_DIRECTORY_OP_REMOVE, path, path_size);
}

struct filesystem_call_result filesystem_directory_rename(cap_id_t directory_cap, const void* old_path,
                                                          size_t old_path_size, const void* new_path,
                                                          size_t new_path_size) {
	struct filesystem_directory_rename_request* request;
	struct filesystem_response_header           response;
	struct filesystem_call_result               call_result;
	size_t                                      request_size;

	if (directory_cap == CAP_ID_INVALID || old_path_size > CAP_MAX_REQUEST_SIZE - sizeof(*request) ||
	    new_path_size > CAP_MAX_REQUEST_SIZE - sizeof(*request) - old_path_size ||
	    !filesystem_relative_path_valid(old_path, old_path_size) ||
	    !filesystem_relative_path_valid(new_path, new_path_size))
		return local_error();
	request_size = sizeof(*request) + old_path_size + new_path_size;
	request      = malloc(request_size);
	if (request == NULL) return result(FILESYSTEM_STATUS_NONE, SYSCALL_STATUS_FAILED);
	*request = (struct filesystem_directory_rename_request){
		.header        = {.op = FILESYSTEM_DIRECTORY_OP_RENAME},
		.old_path_size = (uint32_t)old_path_size,
		.new_path_size = (uint32_t)new_path_size,
		.reserved      = 0u,
	};
	memcpy(request->paths, old_path, old_path_size);
	memcpy(request->paths + old_path_size, new_path, new_path_size);
	call_result = call_fixed(directory_cap, request, request_size, &response, sizeof(response));
	free(request);
	return call_result;
}
