#include <runtime/blob.h>
#include <system/capability.h>

syscall_status_t blob_get_info(cap_id_t blob_cap, struct blob_info_response* out_info) {
	struct blob_info_request req = {
		.header =
			{
					 .op = BLOB_OP_INFO,
					 },
	};
	return cap_call(blob_cap, &req, sizeof(req), out_info, sizeof(*out_info), NULL);
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
	return cap_call(blob_cap, &req, sizeof(req), buffer, size, NULL);
}
