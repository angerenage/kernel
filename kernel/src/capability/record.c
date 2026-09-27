#include "record.h"

#include <kernel/capability.h>
#include <stddef.h>
#include <string.h>

syscall_result_t kernel_record_request_decode(const struct cap_request* request, size_t record_size,
                                              struct kernel_record_request* out_request) {
	struct record_request_header header;

	if (request == NULL || request->request == NULL || request->request_size < sizeof(header) || record_size == 0u ||
	    record_size > CAP_MAX_RESPONSE_SIZE || out_request == NULL) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	if ((request->rights & CAP_READ) == 0u) return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	memcpy(&header, request->request, sizeof(header));
	switch (header.op) {
	case RECORD_OP_COUNT: {
		if (request->request_size != sizeof(struct record_count_request) ||
		    !cap_kernel_response_fits(request, sizeof(struct record_count_response))) {
			return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
		}
		*out_request = (struct kernel_record_request){.op = RECORD_OP_COUNT};
		return syscall_result_ok(0u);
	}
	case RECORD_OP_GET: {
		struct record_get_request get_request;

		if (request->request_size != sizeof(get_request) || !cap_kernel_response_fits(request, record_size)) {
			return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
		}
		memcpy(&get_request, request->request, sizeof(get_request));
		if (get_request.index > SIZE_MAX) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
		*out_request = (struct kernel_record_request){.op = RECORD_OP_GET, .index = (size_t)get_request.index};
		return syscall_result_ok(0u);
	}
	default:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
}

syscall_result_t kernel_record_count_respond(const struct cap_request* request, size_t count) {
	const struct record_count_response response = {.count = count};

	return cap_kernel_write_response(request, &response, sizeof(response));
}
