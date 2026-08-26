#include <base/cap.h>
#include <base/kernel_resource.h>
#include <base/math.h>
#include <libc/stdlib.h>
#include <runtime/diagnostic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <system/capability.h>
#include <system/kernel_resource.h>

syscall_status_t kernel_resources_list(cap_id_t kernel_resources_cap, uint64_t offset, enum kernel_resource_type* ids,
                                       size_t capacity, uint64_t* out_total, size_t* out_returned) {
	struct kernel_resources_list_request request = {
		.header   = {.op = KERNEL_RESOURCES_OP_LIST},
		.offset   = offset,
		.capacity = capacity,
	};
	struct kernel_resources_list_response* response;
	syscall_result_t                       result;
	size_t                                 ids_size;
	size_t                                 response_size;

	if (kernel_resources_cap == CAP_ID_INVALID) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(kernel_resources_cap);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if (capacity != 0u && ids == NULL) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(ids);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if (out_total == NULL) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(out_total);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if (out_returned == NULL) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(out_returned);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if (mul_overflow_size(capacity, sizeof(*ids), &ids_size) ||
	    add_overflow_size(sizeof(*response), ids_size, &response_size) || response_size > CAP_MAX_RESPONSE_SIZE) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(capacity);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}

	response = malloc(response_size);
	if (response == NULL) return SYSCALL_STATUS_FAILED;
	result = cap_call_syscall(kernel_resources_cap, &request, sizeof(request), response, response_size);
	RUNTIME_DIAGNOSTIC_OPERATION_RESULT(KERNEL_RESOURCES_OP_LIST, result);
	if (result.status != SYSCALL_STATUS_OK) {
		free(response);
		return result.status;
	}
	if (result.value < sizeof(*response) || result.value > response_size ||
	    (result.value - sizeof(*response)) % sizeof(*ids) != 0u || response->returned > capacity ||
	    response->returned != (result.value - sizeof(*response)) / sizeof(*ids) ||
	    response->returned > response->total || offset > response->total ||
	    response->returned > response->total - offset) {
		free(response);
		RUNTIME_DIAGNOSTIC_INVALID_STATE("KERNEL_RESOURCES_OP_LIST returned an invalid response");
		return SYSCALL_STATUS_FAILED;
	}
	if (response->returned != 0u) memcpy(ids, response->ids, (size_t)response->returned * sizeof(*ids));
	*out_total    = response->total;
	*out_returned = (size_t)response->returned;
	free(response);
	return SYSCALL_STATUS_OK;
}

syscall_status_t kernel_resource_acquire(cap_id_t kernel_resources_cap, enum kernel_resource_type id,
                                         cap_id_t* out_cap) {
	const struct kernel_resource_acquire_request request = {
		.header = {.op = KERNEL_RESOURCES_OP_ACQUIRE},
		.id     = id,
	};
	struct kernel_resource_acquire_response response;
	syscall_result_t                        result;

	if (kernel_resources_cap == CAP_ID_INVALID) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(kernel_resources_cap);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if (id == KERNEL_RESOURCE_TYPE_INVALID) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(id);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if (out_cap == NULL) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(out_cap);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}

	result = cap_call_syscall(kernel_resources_cap, &request, sizeof(request), &response, sizeof(response));
	RUNTIME_DIAGNOSTIC_OPERATION_RESULT(KERNEL_RESOURCES_OP_ACQUIRE, result);
	if (result.status != SYSCALL_STATUS_OK) return result.status;
	if (result.value != sizeof(response) || response.cap == CAP_ID_INVALID) {
		RUNTIME_DIAGNOSTIC_INVALID_STATE("KERNEL_RESOURCES_OP_ACQUIRE returned an invalid response");
		return SYSCALL_STATUS_FAILED;
	}
	*out_cap = response.cap;
	return SYSCALL_STATUS_OK;
}
