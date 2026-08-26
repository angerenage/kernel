#include "kernel_resource.h"

#include <base/cap.h>
#include <base/kernel_resource.h>
#include <base/math.h>
#include <base/syscall.h>
#include <core/capability.h>
#include <kernel/boot.h>
#include <kernel/capability.h>
#include <libc/stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "boot_module.h"
#include "boot_resource.h"
#include "loader.h"
#include "serial.h"

static cap_object_id_t kernel_resources_object_id = CAP_OBJECT_ID_INVALID;

static size_t kernel_resources_available(enum kernel_resource_type* ids, size_t capacity) {
	size_t count = 0u;

	if (kernel_boot_module_count() != 0u) {
		if (ids != NULL && count < capacity) ids[count] = KERNEL_RESOURCE_TYPE_MODULES;
		count++;
	}
	if (kernel_capability_serial_available()) {
		if (ids != NULL && count < capacity) ids[count] = KERNEL_RESOURCE_TYPE_SERIAL;
		count++;
	}
	if (kernel_capability_loader_available()) {
		if (ids != NULL && count < capacity) ids[count] = KERNEL_RESOURCE_TYPE_LOADER;
		count++;
	}
	if (kernel_capability_framebuffer_available()) {
		if (ids != NULL && count < capacity) ids[count] = KERNEL_RESOURCE_TYPE_FRAMEBUFFER;
		count++;
	}
	if (kernel_capability_boot_data_available(KERNEL_RESOURCE_TYPE_RSDP)) {
		if (ids != NULL && count < capacity) ids[count] = KERNEL_RESOURCE_TYPE_RSDP;
		count++;
	}
	if (kernel_capability_boot_data_available(KERNEL_RESOURCE_TYPE_DTB)) {
		if (ids != NULL && count < capacity) ids[count] = KERNEL_RESOURCE_TYPE_DTB;
		count++;
	}
	return count;
}

static syscall_result_t kernel_resources_list_handler(const struct cap_request* req) {
	struct kernel_resources_list_request   request;
	struct kernel_resources_list_response* response;
	enum kernel_resource_type              available[6];
	size_t                                 available_count;
	size_t                                 start;
	size_t                                 returned;
	size_t                                 ids_size;
	size_t                                 response_size;
	syscall_result_t                       result;

	if ((req->rights & CAP_READ) != CAP_READ) return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	if (req->request == NULL || req->request_size != sizeof(request)) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	memcpy(&request, req->request, sizeof(request));
	if (request.header.op != KERNEL_RESOURCES_OP_LIST || request.capacity > SIZE_MAX || request.offset > SIZE_MAX) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	available_count = kernel_resources_available(available, sizeof(available) / sizeof(available[0]));
	start           = (size_t)request.offset;
	returned        = start < available_count ? available_count - start : 0u;
	if (returned > (size_t)request.capacity) returned = (size_t)request.capacity;
	if (mul_overflow_size(returned, sizeof(*available), &ids_size) ||
	    add_overflow_size(sizeof(*response), ids_size, &response_size) ||
	    !cap_kernel_response_fits(req, response_size)) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	response = malloc(response_size);
	if (response == NULL) return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	response->total    = available_count;
	response->returned = returned;
	if (returned != 0u) memcpy(response->ids, available + start, ids_size);
	result = cap_kernel_write_response(req, response, response_size);
	free(response);
	return result;
}

static syscall_result_t kernel_resource_acquire_handler(const struct cap_request* req) {
	struct kernel_resource_acquire_request  request;
	struct kernel_resource_acquire_response response = {.cap = CAP_ID_INVALID};
	syscall_result_t                        result;

	if ((req->rights & CAP_READ) != CAP_READ) return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	if (req->request == NULL || req->request_size != sizeof(request) ||
	    !cap_kernel_response_fits(req, sizeof(response))) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	memcpy(&request, req->request, sizeof(request));
	if (request.header.op != KERNEL_RESOURCES_OP_ACQUIRE || request.id == KERNEL_RESOURCE_TYPE_INVALID) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	switch (request.id) {
	case KERNEL_RESOURCE_TYPE_MODULES:
		if (kernel_boot_module_count() == 0u) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
		response.cap = kernel_capability_boot_module_provider_grant(req->caller);
		break;
	case KERNEL_RESOURCE_TYPE_SERIAL:
		if (!kernel_capability_serial_available()) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
		response.cap = kernel_capability_serial_grant(req->caller);
		break;
	case KERNEL_RESOURCE_TYPE_LOADER:
		if (!kernel_capability_loader_available()) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
		response.cap = kernel_capability_loader_grant(req->caller);
		break;
	case KERNEL_RESOURCE_TYPE_FRAMEBUFFER:
		if (!kernel_capability_framebuffer_available()) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
		response.cap = kernel_capability_framebuffer_grant(req->caller);
		break;
	case KERNEL_RESOURCE_TYPE_RSDP:
	case KERNEL_RESOURCE_TYPE_DTB:
		if (!kernel_capability_boot_data_available(request.id))
			return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
		response.cap = kernel_capability_boot_data_grant(request.id, req->caller);
		break;
	default:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	if (response.cap == CAP_ID_INVALID) return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	result = cap_kernel_write_response(req, &response, sizeof(response));
	if (result.status != SYSCALL_STATUS_OK) (void)cap_destroy_by_id(response.cap);
	return result;
}

static syscall_result_t kernel_resources_handler(const struct cap_request* req) {
	uint32_t operation;

	if (req->request == NULL || req->request_size < sizeof(operation)) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	memcpy(&operation, req->request, sizeof(operation));
	switch (operation) {
	case KERNEL_RESOURCES_OP_LIST:
		return kernel_resources_list_handler(req);
	case KERNEL_RESOURCES_OP_ACQUIRE:
		return kernel_resource_acquire_handler(req);
	default:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
}

void kernel_capability_resources_init(void) {
	kernel_resources_object_id = cap_object_create_kernel(0u, kernel_resources_handler, NULL);
}

cap_id_t kernel_capability_resources_grant(process_id_t recipient) {
	if (kernel_resources_object_id == CAP_OBJECT_ID_INVALID) return CAP_ID_INVALID;
	return cap_create(kernel_resources_object_id, recipient, CAP_CALL | CAP_READ | CAP_DELEGATE, NULL);
}
