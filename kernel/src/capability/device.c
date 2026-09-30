#include "device.h"

#include <base/cap.h>
#include <base/device.h>
#include <base/math.h>
#include <base/syscall.h>
#include <core/capability.h>
#include <kernel/capability.h>
#include <kernel/device.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define DEVICES_RESOURCE_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_READ | CAP_DELEGATE))

static cap_object_id_t devices_object_id = CAP_OBJECT_ID_INVALID;

static syscall_result_t devices_count_handler(const struct cap_request* req) {
	struct kernel_devices_count_request  request;
	struct kernel_devices_count_response response;
	size_t                               count;

	if ((req->rights & CAP_READ) == 0u) return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	if (req->request == NULL || req->request_size != sizeof(request) ||
	    !cap_kernel_response_fits(req, sizeof(response)))
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	memcpy(&request, req->request, sizeof(request));
	if (request.header.op != KERNEL_DEVICES_OP_COUNT || !kernel_device_count(request.type, &count))
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	response.count = count;
	return cap_kernel_write_response(req, &response, sizeof(response));
}

static syscall_result_t devices_list_handler(const struct cap_request* req) {
	struct kernel_devices_list_request   request;
	struct kernel_devices_list_response* response;
	size_t                               registered_size;
	size_t                               length;
	size_t                               response_entries_size;
	size_t                               response_capacity;
	size_t                               returned;
	size_t                               returned_size;
	size_t                               response_size;

	if ((req->rights & CAP_READ) == 0u) return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	if (req->request == NULL || req->request_size != sizeof(request))
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	memcpy(&request, req->request, sizeof(request));
	if (request.header.op != KERNEL_DEVICES_OP_LIST || request.offset > SIZE_MAX || request.length > SIZE_MAX ||
	    request.element_size > SIZE_MAX || !kernel_device_type_size(request.type, &registered_size) ||
	    request.element_size != registered_size)
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	length = (size_t)request.length;
	if (mul_overflow_size(length, registered_size, &response_entries_size) ||
	    add_overflow_size(sizeof(*response), response_entries_size, &response_capacity) ||
	    response_capacity > CAP_MAX_RESPONSE_SIZE || !cap_kernel_response_fits(req, response_capacity))
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	response = req->response;
	if (!kernel_device_list(
			request.type, (size_t)request.offset, length, registered_size, response->entries, &returned) ||
	    mul_overflow_size(returned, registered_size, &returned_size) ||
	    add_overflow_size(sizeof(*response), returned_size, &response_size))
		return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	response->returned = returned;
	return syscall_result_ok(response_size);
}

static syscall_result_t devices_handler(const struct cap_request* req) {
	struct kernel_devices_request_header header;

	if (req == NULL || req->request == NULL || req->request_size < sizeof(header))
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	memcpy(&header, req->request, sizeof(header));
	switch (header.op) {
	case KERNEL_DEVICES_OP_COUNT:
		return devices_count_handler(req);
	case KERNEL_DEVICES_OP_LIST:
		return devices_list_handler(req);
	default:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
}

bool kernel_capability_devices_init(void) {
	kernel_device_freeze();
	devices_object_id = cap_object_create_kernel(0u, devices_handler, NULL);
	return devices_object_id != CAP_OBJECT_ID_INVALID;
}

cap_id_t kernel_capability_devices_grant(process_id_t recipient) {
	if (devices_object_id == CAP_OBJECT_ID_INVALID || recipient == PROCESS_PID_INVALID) return CAP_ID_INVALID;
	return cap_create(devices_object_id, recipient, DEVICES_RESOURCE_RIGHTS, NULL);
}
