#include <libc/stdlib.h>
#include <libc/string.h>
#include <runtime/stream.h>
#include <system/capability.h>

syscall_status_t stream_read(cap_id_t stream_cap, void* buffer, size_t size, size_t* out_read) {
	struct stream_read_request request = {
		.header   = {.op = STREAM_OP_READ},
		.reserved = 0u,
	};
	size_t           response_size = 0u;
	syscall_status_t status;

	if (out_read == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	*out_read = 0u;
	if (stream_cap == CAP_ID_INVALID || (size != 0u && buffer == NULL)) return SYSCALL_STATUS_BAD_ARGUMENT;

	request.size = size > CAP_MAX_RESPONSE_SIZE ? CAP_MAX_RESPONSE_SIZE : size;
	status       = cap_call(stream_cap, &request, sizeof(request), buffer, (size_t)request.size, &response_size);
	if (status != SYSCALL_STATUS_OK) return status;
	if (response_size > request.size) return SYSCALL_STATUS_FAILED;

	*out_read = response_size;
	return SYSCALL_STATUS_OK;
}

syscall_status_t stream_write(cap_id_t stream_cap, const void* data, size_t size, size_t* out_written) {
	const size_t                 maximum_size = CAP_MAX_REQUEST_SIZE - sizeof(struct stream_write_request);
	struct stream_write_request* request;
	struct stream_write_request  request_header;
	struct stream_write_response response = {0};
	size_t                       request_size;
	size_t                       response_size = 0u;
	size_t                       transfer_size;
	syscall_status_t             status;

	if (out_written == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	*out_written = 0u;
	if (stream_cap == CAP_ID_INVALID || (size != 0u && data == NULL)) return SYSCALL_STATUS_BAD_ARGUMENT;

	transfer_size = size > maximum_size ? maximum_size : size;
	request_size  = sizeof(*request) + transfer_size;
	request       = &request_header;
	if (transfer_size != 0u) {
		request = malloc(request_size);
		if (request == NULL) return SYSCALL_STATUS_FAILED;
	}
	*request = (struct stream_write_request){
		.header   = {.op = STREAM_OP_WRITE},
		.reserved = 0u,
		.size     = transfer_size,
	};
	if (transfer_size != 0u) memcpy(request->data, data, transfer_size);

	status = cap_call(stream_cap, request, request_size, &response, sizeof(response), &response_size);
	if (transfer_size != 0u) free(request);
	if (status != SYSCALL_STATUS_OK) return status;
	if (response_size != sizeof(response) || response.size > transfer_size ||
	    (transfer_size != 0u && response.size == 0u)) {
		return SYSCALL_STATUS_FAILED;
	}

	*out_written = (size_t)response.size;
	return SYSCALL_STATUS_OK;
}
