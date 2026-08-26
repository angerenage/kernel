#include <base/boot_data.h>
#include <base/cap.h>
#include <runtime/diagnostic.h>
#include <stddef.h>
#include <stdint.h>
#include <system/boot_data.h>
#include <system/capability.h>

syscall_status_t boot_data_get_info(cap_id_t boot_data_cap, struct boot_data_info_response* out_info) {
	const struct boot_data_info_request request = {.header = {.op = BOOT_DATA_OP_INFO}};
	syscall_result_t                    result;

	if (boot_data_cap == CAP_ID_INVALID) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(boot_data_cap);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if (out_info == NULL) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(out_info);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	result = cap_call_syscall(boot_data_cap, &request, sizeof(request), out_info, sizeof(*out_info));
	RUNTIME_DIAGNOSTIC_OPERATION_RESULT(BOOT_DATA_OP_INFO, result);
	if (result.status != SYSCALL_STATUS_OK) return result.status;
	if (result.value != sizeof(*out_info) ||
	    (out_info->type != KERNEL_RESOURCE_TYPE_RSDP && out_info->type != KERNEL_RESOURCE_TYPE_DTB) ||
	    out_info->size == 0u) {
		RUNTIME_DIAGNOSTIC_INVALID_STATE("BOOT_DATA_OP_INFO returned an invalid response");
		return SYSCALL_STATUS_FAILED;
	}
	return SYSCALL_STATUS_OK;
}

syscall_status_t boot_data_read(cap_id_t boot_data_cap, uint64_t offset, void* buffer, size_t size) {
	const struct boot_data_read_request request = {
		.header = {.op = BOOT_DATA_OP_READ},
		.offset = offset,
		.size   = size,
	};
	syscall_result_t result;

	if (boot_data_cap == CAP_ID_INVALID) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(boot_data_cap);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if (size != 0u && buffer == NULL) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(buffer);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if (size > CAP_MAX_RESPONSE_SIZE) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(size);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	result = cap_call_syscall(boot_data_cap, &request, sizeof(request), buffer, size);
	RUNTIME_DIAGNOSTIC_OPERATION_RESULT(BOOT_DATA_OP_READ, result);
	if (result.status != SYSCALL_STATUS_OK) return result.status;
	if (result.value != size) {
		RUNTIME_DIAGNOSTIC_INVALID_STATE("BOOT_DATA_OP_READ returned an invalid response size");
		return SYSCALL_STATUS_FAILED;
	}
	return SYSCALL_STATUS_OK;
}
