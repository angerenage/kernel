#include <criterion/criterion.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Keep the runtime implementation separate from the host's stdio namespace. */
#define libc_file runtime_stdio_file
#define FILE runtime_stdio_FILE
#define stdin runtime_stdio_stdin
#define stdout runtime_stdio_stdout
#define stderr runtime_stdio_stderr
#define fopen runtime_stdio_fopen
#define fclose runtime_stdio_fclose
#define fread runtime_stdio_fread
#define fwrite runtime_stdio_fwrite
#define feof runtime_stdio_feof
#define ferror runtime_stdio_ferror
#define clearerr runtime_stdio_clearerr
#define fflush runtime_stdio_fflush
#define calloc runtime_stdio_calloc
#define free runtime_stdio_free
#include "../../userspace/runtime/stdio.c"
#undef free
#undef calloc
#undef fflush
#undef clearerr
#undef ferror
#undef feof
#undef fwrite
#undef fread
#undef fclose
#undef fopen
#undef stderr
#undef stdout
#undef stdin
#undef FILE
#undef libc_file

enum {
	MOCK_VFS_CAP  = 40u,
	MOCK_FILE_CAP = 41u,
	MOCK_MAX_IO   = 8u,
};

struct io_step {
	struct filesystem_call_result result;
	size_t                        transferred;
};

static struct {
	struct init_call_result       acquire_result;
	struct filesystem_call_result open_result;
	struct filesystem_call_result create_result;
	struct filesystem_call_result resize_result;
	struct filesystem_node_handle open_node;
	struct filesystem_node_handle create_node;
	syscall_status_t              drop_status;
	bool                          allocation_fails;
	size_t                        acquire_calls;
	size_t                        open_calls;
	size_t                        create_calls;
	size_t                        resize_calls;
	size_t                        drop_calls;
	cap_id_t                      dropped[4];
	cap_rights_t                  open_rights;
	cap_rights_t                  create_rights;
	uint64_t                      resize_size;
	char                          path[64];
	size_t                        path_size;
	struct io_step                reads[MOCK_MAX_IO];
	struct io_step                writes[MOCK_MAX_IO];
	size_t                        read_count;
	size_t                        write_count;
	size_t                        read_index;
	size_t                        write_index;
	uint64_t                      read_offsets[MOCK_MAX_IO];
	uint64_t                      write_offsets[MOCK_MAX_IO];
	size_t                        read_sizes[MOCK_MAX_IO];
	size_t                        write_sizes[MOCK_MAX_IO];
	const uint8_t*                read_data;
	size_t                        read_data_offset;
	syscall_status_t              display_read_status;
	syscall_status_t              display_write_status;
	syscall_status_t              display_error_status;
	const uint8_t*                display_read_data;
	size_t                        display_read_size;
	size_t                        display_read_offset;
	size_t                        display_read_calls;
	size_t                        display_write_calls;
	size_t                        display_error_calls;
	uint8_t                       display_output[64];
	size_t                        display_output_size;
} mock;

static struct filesystem_call_result filesystem_result(enum filesystem_status status,
                                                       syscall_status_t       transport_status) {
	return (struct filesystem_call_result){.status = status, .transport_status = transport_status};
}

static void mock_reset(void) {
	memset(&mock, 0, sizeof(mock));
	mock.acquire_result = (struct init_call_result){.status = INIT_REGISTRY_OK, .transport_status = SYSCALL_STATUS_OK};
	mock.open_result    = filesystem_result(FILESYSTEM_STATUS_OK, SYSCALL_STATUS_OK);
	mock.create_result  = filesystem_result(FILESYSTEM_STATUS_OK, SYSCALL_STATUS_OK);
	mock.resize_result  = filesystem_result(FILESYSTEM_STATUS_OK, SYSCALL_STATUS_OK);
	mock.open_node =
		(struct filesystem_node_handle){.info = {.type = FILESYSTEM_NODE_FILE}, .capability = MOCK_FILE_CAP};
	mock.create_node          = mock.open_node;
	mock.drop_status          = SYSCALL_STATUS_OK;
	mock.display_read_status  = SYSCALL_STATUS_OK;
	mock.display_write_status = SYSCALL_STATUS_OK;
	mock.display_error_status = SYSCALL_STATUS_OK;
}

void* runtime_stdio_calloc(size_t count, size_t size) {
	if (mock.allocation_fails) return NULL;
	return calloc(count, size);
}

void runtime_stdio_free(void* allocation) {
	free(allocation);
}

bool vfs_absolute_path_valid(const void* path, size_t path_size) {
	return path != NULL && path_size != 0u && ((const char*)path)[0] == '/';
}

struct init_call_result init_acquire(const struct init_protocol_query* query, const char* service,
                                     struct init_service_handle* out_handle) {
	mock.acquire_calls++;
	cr_assert_str_eq(query->namespace_path, VFS_NAMESPACE);
	cr_assert_str_eq(query->protocol, VFS_PROTOCOL_NAME);
	cr_assert_str_eq(service, VFS_SERVICE_NAME);
	if (mock.acquire_result.transport_status == SYSCALL_STATUS_OK && mock.acquire_result.status == INIT_REGISTRY_OK) {
		*out_handle = (struct init_service_handle){.capability = MOCK_VFS_CAP};
	}
	return mock.acquire_result;
}

static void record_path(const void* path, size_t path_size) {
	mock.path_size = path_size;
	cr_assert_leq(path_size, sizeof(mock.path));
	memcpy(mock.path, path, path_size);
}

struct filesystem_call_result vfs_open(cap_id_t capability, const void* path, size_t path_size, cap_rights_t rights,
                                       struct filesystem_node_handle* out_node) {
	mock.open_calls++;
	cr_assert_eq(capability, MOCK_VFS_CAP);
	mock.open_rights = rights;
	record_path(path, path_size);
	if (mock.open_result.transport_status == SYSCALL_STATUS_OK && mock.open_result.status == FILESYSTEM_STATUS_OK)
		*out_node = mock.open_node;
	return mock.open_result;
}

struct filesystem_call_result vfs_create_file(cap_id_t capability, const void* path, size_t path_size,
                                              cap_rights_t rights, struct filesystem_node_handle* out_node) {
	mock.create_calls++;
	cr_assert_eq(capability, MOCK_VFS_CAP);
	mock.create_rights = rights;
	record_path(path, path_size);
	if (mock.create_result.transport_status == SYSCALL_STATUS_OK && mock.create_result.status == FILESYSTEM_STATUS_OK)
		*out_node = mock.create_node;
	return mock.create_result;
}

struct filesystem_call_result filesystem_file_resize(cap_id_t capability, uint64_t size) {
	mock.resize_calls++;
	cr_assert_eq(capability, MOCK_FILE_CAP);
	mock.resize_size = size;
	return mock.resize_result;
}

struct filesystem_call_result filesystem_file_read(cap_id_t capability, uint64_t offset, void* buffer, size_t size,
                                                   size_t* out_read) {
	struct io_step step;

	cr_assert_eq(capability, MOCK_FILE_CAP);
	cr_assert_lt(mock.read_index, mock.read_count);
	step                               = mock.reads[mock.read_index];
	mock.read_offsets[mock.read_index] = offset;
	mock.read_sizes[mock.read_index]   = size;
	mock.read_index++;
	*out_read = step.transferred;
	if (step.result.transport_status == SYSCALL_STATUS_OK && step.result.status == FILESYSTEM_STATUS_OK &&
	    step.transferred <= size) {
		memcpy(buffer, mock.read_data + mock.read_data_offset, step.transferred);
		mock.read_data_offset += step.transferred;
	}
	return step.result;
}

struct filesystem_call_result filesystem_file_write(cap_id_t capability, uint64_t offset, const void* buffer,
                                                    size_t size, size_t* out_written) {
	struct io_step step;

	(void)buffer;
	cr_assert_eq(capability, MOCK_FILE_CAP);
	cr_assert_lt(mock.write_index, mock.write_count);
	step                                 = mock.writes[mock.write_index];
	mock.write_offsets[mock.write_index] = offset;
	mock.write_sizes[mock.write_index]   = size;
	mock.write_index++;
	*out_written = step.transferred;
	return step.result;
}

syscall_status_t cap_drop(cap_id_t capability) {
	cr_assert_lt(mock.drop_calls, sizeof(mock.dropped) / sizeof(mock.dropped[0]));
	mock.dropped[mock.drop_calls++] = capability;
	return mock.drop_status;
}

syscall_status_t display_read(void* data, size_t capacity, size_t* out_read) {
	size_t remaining;
	size_t transferred;

	mock.display_read_calls++;
	*out_read = 0u;
	if (mock.display_read_status != SYSCALL_STATUS_OK) return mock.display_read_status;
	remaining   = mock.display_read_size - mock.display_read_offset;
	transferred = capacity < remaining ? capacity : remaining;
	memcpy(data, mock.display_read_data + mock.display_read_offset, transferred);
	mock.display_read_offset += transferred;
	*out_read = transferred;
	return SYSCALL_STATUS_OK;
}

static syscall_status_t display_write_common(const void* data, size_t size, bool error_stream) {
	syscall_status_t status = error_stream ? mock.display_error_status : mock.display_write_status;

	if (error_stream) mock.display_error_calls++;
	else mock.display_write_calls++;
	if (status != SYSCALL_STATUS_OK) return status;
	cr_assert_leq(mock.display_output_size + size, sizeof(mock.display_output));
	memcpy(mock.display_output + mock.display_output_size, data, size);
	mock.display_output_size += size;
	return SYSCALL_STATUS_OK;
}

syscall_status_t display_write(const char* data, size_t size) {
	return display_write_common(data, size, false);
}

syscall_status_t display_error_write(const char* data, size_t size) {
	return display_write_common(data, size, true);
}

Test(runtime_stdio, parses_modes_requests_rights_and_releases_capabilities) {
	runtime_stdio_FILE* stream;

	mock_reset();
	stream = runtime_stdio_fopen("/boot/catalog", "rb");
	cr_assert_not_null(stream);
	cr_assert_eq(mock.open_rights, CAP_READ);
	cr_assert_eq(mock.create_calls, 0u);
	cr_assert_eq(mock.resize_calls, 0u);
	cr_assert_eq(mock.drop_calls, 1u);
	cr_assert_eq(mock.dropped[0], MOCK_VFS_CAP);
	cr_assert_eq(runtime_stdio_fclose(stream), 0);
	cr_assert_eq(mock.dropped[1], MOCK_FILE_CAP);

	mock_reset();
	stream = runtime_stdio_fopen("/boot/catalog", "wb+");
	cr_assert_not_null(stream);
	cr_assert_eq(mock.open_rights, CAP_READ | CAP_WRITE);
	cr_assert_eq(mock.resize_calls, 1u);
	cr_assert_eq(mock.resize_size, 0u);
	cr_assert_eq(runtime_stdio_fclose(stream), 0);

	mock_reset();
	cr_assert_null(runtime_stdio_fopen("relative", "r"));
	cr_assert_null(runtime_stdio_fopen("/file", "rr"));
	cr_assert_null(runtime_stdio_fopen("/file", "r++"));
	cr_assert_null(runtime_stdio_fopen("/file", ""));
	cr_assert_eq(mock.acquire_calls, 0u);
}

Test(runtime_stdio, creates_missing_files_and_appends_from_initial_size) {
	static const struct io_step successful_write = {
		.result = {.status = FILESYSTEM_STATUS_OK, .transport_status = SYSCALL_STATUS_OK},
          .transferred = 3u
    };
	runtime_stdio_FILE* stream;

	mock_reset();
	mock.open_result           = filesystem_result(FILESYSTEM_STATUS_NOT_FOUND, SYSCALL_STATUS_OK);
	mock.create_node.info.size = 19u;
	mock.write_count           = 1u;
	mock.writes[0]             = successful_write;
	stream                     = runtime_stdio_fopen("/new", "ab");
	cr_assert_not_null(stream);
	cr_assert_eq(mock.create_calls, 1u);
	cr_assert_eq(mock.open_rights, CAP_WRITE);
	cr_assert_eq(mock.create_rights, CAP_WRITE);
	cr_assert_eq(runtime_stdio_fwrite("abc", 1u, 3u, stream), 3u);
	cr_assert_eq(mock.write_offsets[0], 19u);
	cr_assert_eq(runtime_stdio_fclose(stream), 0);
}

Test(runtime_stdio, cleans_up_after_open_create_and_truncate_failures) {
	mock_reset();
	mock.open_result = filesystem_result(FILESYSTEM_STATUS_NOT_FOUND, SYSCALL_STATUS_OK);
	cr_assert_null(runtime_stdio_fopen("/missing", "r"));
	cr_assert_eq(mock.create_calls, 0u);
	cr_assert_eq(mock.drop_calls, 1u);
	cr_assert_eq(mock.dropped[0], MOCK_VFS_CAP);

	mock_reset();
	mock.open_result   = filesystem_result(FILESYSTEM_STATUS_NOT_FOUND, SYSCALL_STATUS_OK);
	mock.create_result = filesystem_result(FILESYSTEM_STATUS_NO_SPACE, SYSCALL_STATUS_OK);
	cr_assert_null(runtime_stdio_fopen("/missing", "w"));
	cr_assert_eq(mock.create_calls, 1u);
	cr_assert_eq(mock.drop_calls, 1u);
	cr_assert_eq(mock.dropped[0], MOCK_VFS_CAP);

	mock_reset();
	mock.resize_result = filesystem_result(FILESYSTEM_STATUS_IO_ERROR, SYSCALL_STATUS_OK);
	cr_assert_null(runtime_stdio_fopen("/existing", "w"));
	cr_assert_eq(mock.drop_calls, 2u);
	cr_assert_eq(mock.dropped[0], MOCK_FILE_CAP);
	cr_assert_eq(mock.dropped[1], MOCK_VFS_CAP);
}

Test(runtime_stdio, loops_partial_reads_tracks_eof_and_reports_complete_elements) {
	static const uint8_t contents[] = "abcde";
	runtime_stdio_FILE*  stream;
	uint8_t              buffer[8] = {0};

	mock_reset();
	mock.read_data  = contents;
	mock.read_count = 3u;
	mock.reads[0]   = (struct io_step){filesystem_result(FILESYSTEM_STATUS_OK, SYSCALL_STATUS_OK), 2u};
	mock.reads[1]   = (struct io_step){filesystem_result(FILESYSTEM_STATUS_OK, SYSCALL_STATUS_OK), 3u};
	mock.reads[2]   = (struct io_step){filesystem_result(FILESYSTEM_STATUS_OK, SYSCALL_STATUS_OK), 0u};
	stream          = runtime_stdio_fopen("/data", "r");
	cr_assert_not_null(stream);
	cr_assert_eq(runtime_stdio_fread(buffer, 2u, 3u, stream), 2u);
	cr_assert_arr_eq(buffer, contents, 5u);
	cr_assert_eq(mock.read_offsets[0], 0u);
	cr_assert_eq(mock.read_offsets[1], 2u);
	cr_assert_eq(mock.read_offsets[2], 5u);
	cr_assert(runtime_stdio_feof(stream));
	cr_assert_not(runtime_stdio_ferror(stream));
	runtime_stdio_clearerr(stream);
	cr_assert_not(runtime_stdio_feof(stream));
	cr_assert_eq(runtime_stdio_fclose(stream), 0);
}

Test(runtime_stdio, handles_io_errors_overflow_and_allocation_cleanup) {
	runtime_stdio_FILE* stream;
	uint8_t             byte = 0u;

	mock_reset();
	mock.read_count = 1u;
	mock.reads[0]   = (struct io_step){filesystem_result(FILESYSTEM_STATUS_IO_ERROR, SYSCALL_STATUS_OK), 0u};
	stream          = runtime_stdio_fopen("/data", "r");
	cr_assert_not_null(stream);
	cr_assert_eq(runtime_stdio_fread(&byte, 1u, 1u, stream), 0u);
	cr_assert(runtime_stdio_ferror(stream));
	cr_assert_not(runtime_stdio_feof(stream));
	runtime_stdio_clearerr(stream);
	cr_assert_eq(runtime_stdio_fread(&byte, SIZE_MAX, 2u, stream), 0u);
	cr_assert(runtime_stdio_ferror(stream));
	cr_assert_eq(runtime_stdio_fclose(stream), 0);

	mock_reset();
	mock.allocation_fails = true;
	cr_assert_null(runtime_stdio_fopen("/data", "r"));
	cr_assert_eq(mock.drop_calls, 2u);
	cr_assert_eq(mock.dropped[0], MOCK_FILE_CAP);
	cr_assert_eq(mock.dropped[1], MOCK_VFS_CAP);
}

Test(runtime_stdio, loops_partial_writes_and_rejects_incompatible_access) {
	runtime_stdio_FILE* stream;

	mock_reset();
	mock.write_count = 2u;
	mock.writes[0]   = (struct io_step){filesystem_result(FILESYSTEM_STATUS_OK, SYSCALL_STATUS_OK), 1u};
	mock.writes[1]   = (struct io_step){filesystem_result(FILESYSTEM_STATUS_OK, SYSCALL_STATUS_OK), 3u};
	stream           = runtime_stdio_fopen("/data", "w");
	cr_assert_not_null(stream);
	cr_assert_eq(runtime_stdio_fwrite("data", 2u, 2u, stream), 2u);
	cr_assert_eq(mock.write_offsets[0], 0u);
	cr_assert_eq(mock.write_offsets[1], 1u);
	cr_assert_eq(mock.write_sizes[0], 4u);
	cr_assert_eq(mock.write_sizes[1], 3u);
	cr_assert_eq(runtime_stdio_fread((char[1]){0}, 1u, 1u, stream), 0u);
	cr_assert(runtime_stdio_ferror(stream));
	cr_assert_eq(runtime_stdio_fclose(stream), 0);
}

Test(runtime_stdio, routes_the_static_standard_streams) {
	static const uint8_t input[]   = "in";
	uint8_t              buffer[3] = {0};

	mock_reset();
	mock.display_read_data = input;
	mock.display_read_size = sizeof(input) - 1u;
	cr_assert_eq(runtime_stdio_fread(buffer, 1u, sizeof(buffer), runtime_stdio_stdin), 2u);
	cr_assert_arr_eq(buffer, input, 2u);
	cr_assert(runtime_stdio_feof(runtime_stdio_stdin));
	cr_assert_eq(runtime_stdio_fwrite("out", 1u, 3u, runtime_stdio_stdout), 3u);
	cr_assert_eq(runtime_stdio_fwrite("err", 1u, 3u, runtime_stdio_stderr), 3u);
	cr_assert_eq(mock.display_write_calls, 1u);
	cr_assert_eq(mock.display_error_calls, 1u);
	cr_assert_arr_eq(mock.display_output, "outerr", 6u);
	cr_assert_eq(runtime_stdio_fflush(NULL), 0);
	cr_assert_eq(runtime_stdio_fflush(runtime_stdio_stdout), 0);
	cr_assert_eq(runtime_stdio_fclose(runtime_stdio_stdout), EOF);
	cr_assert(runtime_stdio_ferror(runtime_stdio_stdout));
}
