#include <base/cap.h>
#include <base/syscall.h>
#include <criterion/criterion.h>
#include <protocol/filesystem.h>
#include <protocol/vfs.h>
#include <runtime/filesystem.h>
#include <runtime/vfs.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct cap_call_mock {
	syscall_status_t status;
	cap_id_t         cap;
	size_t           request_size;
	size_t           response_capacity;
	size_t           response_size;
	size_t           calls;
	uint8_t          request[CAP_MAX_REQUEST_SIZE];
	uint8_t          response[CAP_MAX_RESPONSE_SIZE];
};

static struct cap_call_mock mock;

static void mock_reset(void) {
	memset(&mock, 0, sizeof(mock));
	mock.status = SYSCALL_STATUS_OK;
}

static void mock_header(enum filesystem_status status) {
	const struct filesystem_response_header response = {
		.status   = status,
		.reserved = 0u,
	};

	memcpy(mock.response, &response, sizeof(response));
	mock.response_size = sizeof(response);
}

static void mock_open(uint32_t type, cap_id_t capability) {
	const struct filesystem_open_response response = {
		.header     = {.status = FILESYSTEM_STATUS_OK},
		.info       = {.type = type, .size = 17u},
		.capability = capability,
	};

	memcpy(mock.response, &response, sizeof(response));
	mock.response_size = sizeof(response);
}

syscall_status_t cap_call(cap_id_t cap, const void* request, size_t request_size, void* response,
                          size_t response_capacity, size_t* result_value) {
	size_t copy_size;

	mock.calls++;
	mock.cap               = cap;
	mock.request_size      = request_size;
	mock.response_capacity = response_capacity;
	copy_size              = request_size < sizeof(mock.request) ? request_size : sizeof(mock.request);
	if (copy_size != 0u) memcpy(mock.request, request, copy_size);
	if (mock.status != SYSCALL_STATUS_OK) return mock.status;
	copy_size = mock.response_size < response_capacity ? mock.response_size : response_capacity;
	if (copy_size != 0u) memcpy(response, mock.response, copy_size);
	*result_value = mock.response_size;
	return SYSCALL_STATUS_OK;
}

Test(runtime_filesystem, validates_relative_and_absolute_paths_locally) {
	uint8_t                       long_component[FILESYSTEM_NAME_MAX + 1u];
	struct filesystem_node_handle node;
	struct filesystem_call_result result;

	memset(long_component, 'x', sizeof(long_component));
	cr_assert(filesystem_relative_path_valid("boot/kernel", 11u));
	cr_assert_not(filesystem_relative_path_valid("/boot", 5u));
	cr_assert_not(filesystem_relative_path_valid("boot/", 5u));
	cr_assert_not(filesystem_relative_path_valid("boot//kernel", 12u));
	cr_assert_not(filesystem_relative_path_valid(".", 1u));
	cr_assert_not(filesystem_relative_path_valid("boot/../kernel", 14u));
	cr_assert_not(filesystem_relative_path_valid(long_component, sizeof(long_component)));
	cr_assert(vfs_absolute_path_valid("/", 1u));
	cr_assert(vfs_absolute_path_valid("/boot/kernel", 12u));
	cr_assert_not(vfs_absolute_path_valid("boot", 4u));
	cr_assert_not(vfs_absolute_path_valid("//boot", 6u));

	mock_reset();
	node.capability = 123u;
	result          = filesystem_directory_open(4u, "/bad", 4u, CAP_READ, &node);
	cr_assert_eq(result.transport_status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(result.status, FILESYSTEM_STATUS_INVALID_ARGUMENT);
	cr_assert_eq(node.capability, CAP_ID_INVALID);
	cr_assert_eq(mock.calls, 0u);

	result = vfs_open(4u, "relative", 8u, CAP_READ, &node);
	cr_assert_eq(result.transport_status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(mock.calls, 0u);
}

Test(runtime_filesystem, frames_raw_directory_operations) {
	const char                                 path[]     = "boot/kernel";
	const char                                 old_path[] = "old";
	const char                                 new_path[] = "dir/new";
	struct filesystem_directory_open_request   open_request;
	struct filesystem_directory_rename_request rename_request;
	struct filesystem_directory_path_request   path_request;
	struct filesystem_node_handle              node;
	struct filesystem_call_result              result;

	mock_reset();
	mock_open(FILESYSTEM_NODE_FILE, 90u);
	result = filesystem_directory_open(7u, path, sizeof(path) - 1u, CAP_READ | CAP_DELEGATE, &node);
	cr_assert_eq(result.transport_status, SYSCALL_STATUS_OK);
	cr_assert_eq(result.status, FILESYSTEM_STATUS_OK);
	cr_assert_eq(node.info.type, FILESYSTEM_NODE_FILE);
	cr_assert_eq(node.capability, 90u);
	memcpy(&open_request, mock.request, sizeof(open_request));
	cr_assert_eq(mock.calls, 1u);
	cr_assert_eq(mock.cap, 7u);
	cr_assert_eq(mock.request_size, sizeof(open_request) + sizeof(path) - 1u);
	cr_assert_eq(open_request.header.op, FILESYSTEM_DIRECTORY_OP_OPEN);
	cr_assert_eq(open_request.path_size, sizeof(path) - 1u);
	cr_assert_eq(open_request.rights, CAP_READ | CAP_DELEGATE);
	cr_assert_arr_eq(mock.request + sizeof(open_request), path, sizeof(path) - 1u);

	mock_reset();
	mock_open(FILESYSTEM_NODE_DIRECTORY, 91u);
	result = filesystem_directory_create_directory(7u, "new", 3u, CAP_READ | CAP_WRITE, &node);
	cr_assert_eq(result.status, FILESYSTEM_STATUS_OK);
	memcpy(&open_request, mock.request, sizeof(open_request));
	cr_assert_eq(open_request.header.op, FILESYSTEM_DIRECTORY_OP_CREATE_DIRECTORY);
	cr_assert_eq(node.info.type, FILESYSTEM_NODE_DIRECTORY);

	mock_reset();
	mock_header(FILESYSTEM_STATUS_NOT_FOUND);
	result = filesystem_directory_remove(7u, path, sizeof(path) - 1u);
	cr_assert_eq(result.transport_status, SYSCALL_STATUS_OK);
	cr_assert_eq(result.status, FILESYSTEM_STATUS_NOT_FOUND);
	memcpy(&path_request, mock.request, sizeof(path_request));
	cr_assert_eq(path_request.header.op, FILESYSTEM_DIRECTORY_OP_REMOVE);
	cr_assert_eq(path_request.path_size, sizeof(path) - 1u);

	mock_reset();
	mock_header(FILESYSTEM_STATUS_OK);
	result = filesystem_directory_rename(7u, old_path, sizeof(old_path) - 1u, new_path, sizeof(new_path) - 1u);
	cr_assert_eq(result.status, FILESYSTEM_STATUS_OK);
	memcpy(&rename_request, mock.request, sizeof(rename_request));
	cr_assert_eq(rename_request.header.op, FILESYSTEM_DIRECTORY_OP_RENAME);
	cr_assert_eq(rename_request.old_path_size, sizeof(old_path) - 1u);
	cr_assert_eq(rename_request.new_path_size, sizeof(new_path) - 1u);
	cr_assert_eq(rename_request.reserved, 0u);
	cr_assert_arr_eq(mock.request + sizeof(rename_request), old_path, sizeof(old_path) - 1u);
	cr_assert_arr_eq(mock.request + sizeof(rename_request) + sizeof(old_path) - 1u, new_path, sizeof(new_path) - 1u);
}

Test(runtime_filesystem, performs_bounded_positional_io) {
	static uint8_t                        large[CAP_MAX_REQUEST_SIZE];
	struct filesystem_file_read_request   read_request;
	struct filesystem_file_write_request  write_request;
	struct filesystem_file_read_response* read_response = (void*)mock.response;
	struct filesystem_file_write_response write_response;
	struct filesystem_call_result         result;
	uint8_t                               buffer[8] = {0};
	size_t                                transferred;

	mock_reset();
	read_response->header = (struct filesystem_response_header){.status = FILESYSTEM_STATUS_OK};
	memcpy(read_response->data, "abc", 3u);
	mock.response_size = sizeof(*read_response) + 3u;
	transferred        = SIZE_MAX;
	result             = filesystem_file_read(11u, 44u, buffer, sizeof(buffer), &transferred);
	cr_assert_eq(result.status, FILESYSTEM_STATUS_OK);
	cr_assert_eq(transferred, 3u);
	cr_assert_arr_eq(buffer, "abc", 3u);
	memcpy(&read_request, mock.request, sizeof(read_request));
	cr_assert_eq(read_request.header.op, FILESYSTEM_FILE_OP_READ);
	cr_assert_eq(read_request.reserved, 0u);
	cr_assert_eq(read_request.offset, 44u);
	cr_assert_eq(read_request.size, sizeof(buffer));

	mock_reset();
	mock_header(FILESYSTEM_STATUS_OK);
	result = filesystem_file_read(11u, 99u, buffer, sizeof(buffer), &transferred);
	cr_assert_eq(result.status, FILESYSTEM_STATUS_OK);
	cr_assert_eq(transferred, 0u);

	mock_reset();
	mock_header(FILESYSTEM_STATUS_OK);
	result = filesystem_file_read(11u, 0u, large, sizeof(large), &transferred);
	cr_assert_eq(result.status, FILESYSTEM_STATUS_OK);
	memcpy(&read_request, mock.request, sizeof(read_request));
	cr_assert_eq(read_request.size, CAP_MAX_RESPONSE_SIZE - sizeof(struct filesystem_file_read_response));
	cr_assert_eq(mock.response_capacity, CAP_MAX_RESPONSE_SIZE);

	mock_reset();
	write_response = (struct filesystem_file_write_response){
		.header = {.status = FILESYSTEM_STATUS_OK},
		.size   = 2u,
	};
	memcpy(mock.response, &write_response, sizeof(write_response));
	mock.response_size = sizeof(write_response);
	result             = filesystem_file_write(12u, 55u, "data", 4u, &transferred);
	cr_assert_eq(result.status, FILESYSTEM_STATUS_OK);
	cr_assert_eq(transferred, 2u);
	memcpy(&write_request, mock.request, sizeof(write_request));
	cr_assert_eq(write_request.header.op, FILESYSTEM_FILE_OP_WRITE);
	cr_assert_eq(write_request.reserved, 0u);
	cr_assert_eq(write_request.offset, 55u);
	cr_assert_eq(write_request.size, 4u);
	cr_assert_arr_eq(mock.request + sizeof(write_request), "data", 4u);

	mock_reset();
	write_response = (struct filesystem_file_write_response){
		.header = {.status = FILESYSTEM_STATUS_OK},
		.size   = CAP_MAX_REQUEST_SIZE - sizeof(struct filesystem_file_write_request),
	};
	memcpy(mock.response, &write_response, sizeof(write_response));
	mock.response_size = sizeof(write_response);
	result             = filesystem_file_write(12u, 0u, large, sizeof(large), &transferred);
	cr_assert_eq(result.status, FILESYSTEM_STATUS_OK);
	cr_assert_eq(mock.request_size, CAP_MAX_REQUEST_SIZE);
	memcpy(&write_request, mock.request, sizeof(write_request));
	cr_assert_eq(write_request.size, CAP_MAX_REQUEST_SIZE - sizeof(write_request));
}

Test(runtime_filesystem, validates_domain_and_transport_responses) {
	struct filesystem_file_write_response response;
	struct filesystem_node_handle         node;
	struct filesystem_call_result         result;
	size_t                                written = SIZE_MAX;

	mock_reset();
	result = filesystem_file_write(CAP_ID_INVALID, 0u, "data", 4u, &written);
	cr_assert_eq(result.transport_status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(result.status, FILESYSTEM_STATUS_INVALID_ARGUMENT);
	cr_assert_eq(written, 0u);
	cr_assert_eq(mock.calls, 0u);

	mock_reset();
	mock.status = SYSCALL_STATUS_DENIED;
	result      = filesystem_directory_open(2u, "file", 4u, CAP_READ, &node);
	cr_assert_eq(result.transport_status, SYSCALL_STATUS_DENIED);
	cr_assert_eq(result.status, FILESYSTEM_STATUS_NONE);
	cr_assert_eq(node.capability, CAP_ID_INVALID);

	mock_reset();
	mock_header(FILESYSTEM_STATUS_READ_ONLY);
	result = filesystem_file_resize(2u, 10u);
	cr_assert_eq(result.transport_status, SYSCALL_STATUS_OK);
	cr_assert_eq(result.status, FILESYSTEM_STATUS_READ_ONLY);

	mock_reset();
	mock_open(FILESYSTEM_NODE_INVALID, 4u);
	result = filesystem_directory_open(2u, "file", 4u, CAP_READ, &node);
	cr_assert_eq(result.transport_status, SYSCALL_STATUS_FAILED);
	cr_assert_eq(result.status, FILESYSTEM_STATUS_NONE);

	mock_reset();
	mock_open(FILESYSTEM_NODE_FILE, CAP_ID_INVALID);
	result = filesystem_directory_open(2u, "file", 4u, CAP_READ, &node);
	cr_assert_eq(result.transport_status, SYSCALL_STATUS_FAILED);
	cr_assert_eq(node.capability, CAP_ID_INVALID);

	mock_reset();
	response = (struct filesystem_file_write_response){
		.header = {.status = FILESYSTEM_STATUS_OK},
		.size   = 5u,
	};
	memcpy(mock.response, &response, sizeof(response));
	mock.response_size = sizeof(response);
	result             = filesystem_file_write(2u, 0u, "data", 4u, &written);
	cr_assert_eq(result.transport_status, SYSCALL_STATUS_FAILED);
	cr_assert_eq(written, 0u);

	mock_reset();
	mock_header(FILESYSTEM_STATUS_OK);
	((struct filesystem_response_header*)mock.response)->reserved = 1u;
	result                                                        = filesystem_file_resize(2u, 10u);
	cr_assert_eq(result.transport_status, SYSCALL_STATUS_FAILED);

	mock_reset();
	mock.response_size = CAP_MAX_RESPONSE_SIZE + 1u;
	result             = filesystem_file_read(2u, 0u, &response, sizeof(response), &written);
	cr_assert_eq(result.transport_status, SYSCALL_STATUS_FAILED);
	cr_assert_eq(written, 0u);
}

Test(runtime_filesystem, enumerates_with_numeric_offsets_and_validates_entries) {
	struct filesystem_directory_enumerate_response* response = (void*)mock.response;
	struct filesystem_directory_enumerate_request   request;
	struct filesystem_directory_entry               entries[2];
	struct filesystem_call_result                   result;
	size_t                                          returned = SIZE_MAX;
	uint64_t                                        total    = UINT64_MAX;
	const size_t                                    maximum_count =
		(CAP_MAX_RESPONSE_SIZE - sizeof(*response)) / sizeof(struct filesystem_directory_entry);

	mock_reset();
	response->header     = (struct filesystem_response_header){.status = FILESYSTEM_STATUS_OK};
	response->total      = 8u;
	response->returned   = 2u;
	response->entries[0] = (struct filesystem_directory_entry){
		.name_size = 3u,
		.info      = {.type = FILESYSTEM_NODE_FILE, .size = 12u},
	};
	response->entries[1] = (struct filesystem_directory_entry){
		.name_size = 3u,
		.info      = {.type = FILESYSTEM_NODE_DIRECTORY},
	};
	memcpy(response->entries[0].name, "one", 3u);
	memcpy(response->entries[1].name, "two", 3u);
	mock.response_size = sizeof(*response) + 2u * sizeof(*entries);
	result             = filesystem_directory_enumerate(6u, 3u, entries, 2u, &returned, &total);
	cr_assert_eq(result.status, FILESYSTEM_STATUS_OK);
	cr_assert_eq(returned, 2u);
	cr_assert_eq(total, 8u);
	cr_assert_arr_eq(entries[0].name, "one", 3u);
	memcpy(&request, mock.request, sizeof(request));
	cr_assert_eq(request.header.op, FILESYSTEM_DIRECTORY_OP_ENUMERATE);
	cr_assert_eq(request.reserved, 0u);
	cr_assert_eq(request.offset, 3u);
	cr_assert_eq(request.count, 2u);

	mock_reset();
	response->header   = (struct filesystem_response_header){.status = FILESYSTEM_STATUS_OK};
	response->total    = 0u;
	response->returned = 0u;
	mock.response_size = sizeof(*response);
	result             = vfs_directory_enumerate(6u, 0u, entries, SIZE_MAX, &returned, &total);
	cr_assert_eq(result.status, FILESYSTEM_STATUS_OK);
	memcpy(&request, mock.request, sizeof(request));
	cr_assert_eq(request.header.op, VFS_DIRECTORY_OP_ENUMERATE);
	cr_assert_eq(request.count, maximum_count);
	cr_assert_eq(mock.response_capacity, sizeof(*response) + maximum_count * sizeof(*entries));
	cr_assert_leq(mock.response_capacity, CAP_MAX_RESPONSE_SIZE);

	mock_reset();
	response->header   = (struct filesystem_response_header){.status = FILESYSTEM_STATUS_OK};
	response->total    = 1u;
	response->returned = 2u;
	mock.response_size = sizeof(*response);
	result             = filesystem_directory_enumerate(6u, 0u, entries, 2u, &returned, &total);
	cr_assert_eq(result.transport_status, SYSCALL_STATUS_FAILED);
	cr_assert_eq(returned, 0u);
	cr_assert_eq(total, 0u);
}

Test(runtime_filesystem, frames_vfs_namespace_and_mount_operations) {
	const char                         path[] = "/boot/kernel";
	struct vfs_open_request            open_request;
	struct vfs_rename_request          rename_request;
	struct vfs_directory_mount_request mount_request;
	struct filesystem_info_request     unmount_request;
	struct filesystem_node_handle      node;
	struct filesystem_call_result      result;

	mock_reset();
	mock_open(FILESYSTEM_NODE_FILE, 22u);
	result = vfs_open(20u, path, sizeof(path) - 1u, CAP_READ, &node);
	cr_assert_eq(result.status, FILESYSTEM_STATUS_OK);
	memcpy(&open_request, mock.request, sizeof(open_request));
	cr_assert_eq(open_request.header.op, VFS_OP_OPEN);
	cr_assert_eq(open_request.path_size, sizeof(path) - 1u);
	cr_assert_eq(open_request.rights, CAP_READ);
	cr_assert_arr_eq(mock.request + sizeof(open_request), path, sizeof(path) - 1u);

	mock_reset();
	mock_header(FILESYSTEM_STATUS_CROSS_FILESYSTEM);
	result = vfs_rename(20u, "/a", 2u, "/b", 2u);
	cr_assert_eq(result.transport_status, SYSCALL_STATUS_OK);
	cr_assert_eq(result.status, FILESYSTEM_STATUS_CROSS_FILESYSTEM);
	memcpy(&rename_request, mock.request, sizeof(rename_request));
	cr_assert_eq(rename_request.header.op, VFS_OP_RENAME);
	cr_assert_eq(rename_request.reserved, 0u);

	mock_reset();
	mock_header(FILESYSTEM_STATUS_OK);
	result = vfs_directory_mount(30u, 31u, VFS_MOUNT_READ_ONLY);
	cr_assert_eq(result.status, FILESYSTEM_STATUS_OK);
	memcpy(&mount_request, mock.request, sizeof(mount_request));
	cr_assert_eq(mock.cap, 30u);
	cr_assert_eq(mount_request.header.op, VFS_DIRECTORY_OP_MOUNT);
	cr_assert_eq(mount_request.flags, VFS_MOUNT_READ_ONLY);
	cr_assert_eq(mount_request.root_capability, 31u);

	mock_reset();
	mock_header(FILESYSTEM_STATUS_BUSY);
	result = vfs_directory_unmount(30u);
	cr_assert_eq(result.transport_status, SYSCALL_STATUS_OK);
	cr_assert_eq(result.status, FILESYSTEM_STATUS_BUSY);
	memcpy(&unmount_request, mock.request, sizeof(unmount_request));
	cr_assert_eq(unmount_request.header.op, VFS_DIRECTORY_OP_UNMOUNT);
	cr_assert_eq(unmount_request.reserved, 0u);

	mock_reset();
	result = vfs_directory_mount(30u, 31u, UINT32_MAX);
	cr_assert_eq(result.transport_status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(mock.calls, 0u);
}
