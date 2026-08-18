#include <runtime/blob.h>
#include <system/capability.h>

syscall_status_t blob_get_info(cap_id_t blob_cap, struct blob_info_response* out_info) {
	struct blob_info_request req = {
		.header =
			{
					 .op = BLOB_OP_INFO,
					 },
	};
	size_t           response_size = 0u;
	syscall_status_t status        = cap_call(blob_cap, &req, sizeof(req), out_info, sizeof(*out_info), &response_size);
	if (status != SYSCALL_STATUS_OK) return status;
	return response_size == sizeof(*out_info) ? SYSCALL_STATUS_OK : SYSCALL_STATUS_FAILED;
}

syscall_status_t blob_read(cap_id_t blob_cap, uint64_t offset, void* buffer, size_t size) {
	struct blob_read_request req = {
		.header =
			{
					 .op = BLOB_OP_READ,
					 },
		.offset = offset,
		.size   = size,
	};
	size_t           response_size = 0u;
	syscall_status_t status;

	if (size == 0u) return SYSCALL_STATUS_OK;
	status = cap_call(blob_cap, &req, sizeof(req), buffer, size, &response_size);
	if (status != SYSCALL_STATUS_OK) return status;
	return response_size == size ? SYSCALL_STATUS_OK : SYSCALL_STATUS_FAILED;
}
