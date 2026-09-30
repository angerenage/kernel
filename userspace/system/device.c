#include <base/cap.h>
#include <base/device.h>
#include <base/math.h>
#include <base/syscall.h>
#include <libc/stdlib.h>
#include <runtime/diagnostic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <system/capability.h>
#include <system/device.h>

syscall_status_t kernel_devices_count(cap_id_t devices_cap, enum kernel_device_type type, uint64_t* out_count) {
	const struct kernel_devices_count_request request = {
		.header = {.op = KERNEL_DEVICES_OP_COUNT},
		.type   = type,
	};
	struct kernel_devices_count_response response;
	syscall_result_t                     result;

	if (devices_cap == CAP_ID_INVALID || type == KERNEL_DEVICE_TYPE_INVALID || out_count == NULL)
		return SYSCALL_STATUS_BAD_ARGUMENT;
	*out_count = 0u;
	result     = cap_call_syscall(devices_cap, &request, sizeof(request), &response, sizeof(response));
	RUNTIME_DIAGNOSTIC_OPERATION_RESULT(KERNEL_DEVICES_OP_COUNT, result);
	if (result.status != SYSCALL_STATUS_OK) return result.status;
	if (result.value != sizeof(response)) return SYSCALL_STATUS_FAILED;
	*out_count = response.count;
	return SYSCALL_STATUS_OK;
}

syscall_status_t kernel_devices_list(cap_id_t devices_cap, enum kernel_device_type type, uint64_t offset, void* entries,
                                     size_t element_size, size_t length, size_t* out_returned) {
	struct kernel_devices_list_request request = {
		.header       = {.op = KERNEL_DEVICES_OP_LIST},
		.type         = type,
		.offset       = offset,
		.length       = length,
		.element_size = element_size,
	};
	struct kernel_devices_list_response* response;
	syscall_result_t                     result;
	size_t                               entries_size;
	size_t                               response_size;
	size_t                               payload_size;

	if (devices_cap == CAP_ID_INVALID || type == KERNEL_DEVICE_TYPE_INVALID || element_size == 0u ||
	    (length != 0u && entries == NULL) || out_returned == NULL ||
	    mul_overflow_size(length, element_size, &entries_size) ||
	    add_overflow_size(sizeof(*response), entries_size, &response_size) || response_size > CAP_MAX_RESPONSE_SIZE)
		return SYSCALL_STATUS_BAD_ARGUMENT;
	*out_returned = 0u;
	response      = malloc(response_size);
	if (response == NULL) return SYSCALL_STATUS_FAILED;
	result = cap_call_syscall(devices_cap, &request, sizeof(request), response, response_size);
	RUNTIME_DIAGNOSTIC_OPERATION_RESULT(KERNEL_DEVICES_OP_LIST, result);
	if (result.status != SYSCALL_STATUS_OK) {
		free(response);
		return result.status;
	}
	if (result.value < sizeof(*response) || result.value > response_size) {
		free(response);
		return SYSCALL_STATUS_FAILED;
	}
	payload_size = result.value - sizeof(*response);
	if (payload_size % element_size != 0u || response->returned > length ||
	    response->returned != payload_size / element_size) {
		free(response);
		return SYSCALL_STATUS_FAILED;
	}
	if (response->returned != 0u) memcpy(entries, response->entries, payload_size);
	*out_returned = (size_t)response->returned;
	free(response);
	return SYSCALL_STATUS_OK;
}
