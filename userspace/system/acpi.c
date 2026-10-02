#include <base/acpi.h>
#include <base/cap.h>
#include <base/syscall.h>
#include <runtime/diagnostic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <system/acpi.h>
#include <system/capability.h>

#include "syscall.h"

syscall_status_t acpi_table_count(cap_id_t provider_cap, const char signature[4], uint64_t* out_count) {
	struct acpi_provider_count_request  request = {.header = {.op = ACPI_PROVIDER_OP_COUNT}};
	struct acpi_provider_count_response response;
	syscall_result_t                    result;

	if (provider_cap == CAP_ID_INVALID || signature == NULL || out_count == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	memcpy(request.signature, signature, sizeof(request.signature));
	*out_count = 0u;
	result     = cap_call_syscall(provider_cap, &request, sizeof(request), &response, sizeof(response));
	RUNTIME_DIAGNOSTIC_OPERATION_RESULT(ACPI_PROVIDER_OP_COUNT, result);
	if (result.status != SYSCALL_STATUS_OK) return result.status;
	if (result.value != sizeof(response)) return SYSCALL_STATUS_FAILED;
	*out_count = response.count;
	return SYSCALL_STATUS_OK;
}

syscall_status_t acpi_table_claim(cap_id_t provider_cap, const char signature[4], uint64_t index,
                                  cap_id_t* out_table_cap) {
	struct acpi_provider_claim_request  request  = {.header = {.op = ACPI_PROVIDER_OP_CLAIM}, .index = index};
	struct acpi_provider_claim_response response = {.table_cap = CAP_ID_INVALID};
	syscall_result_t                    result;

	if (provider_cap == CAP_ID_INVALID || signature == NULL || out_table_cap == NULL)
		return SYSCALL_STATUS_BAD_ARGUMENT;
	memcpy(request.signature, signature, sizeof(request.signature));
	*out_table_cap = CAP_ID_INVALID;
	result         = cap_call_syscall(provider_cap, &request, sizeof(request), &response, sizeof(response));
	RUNTIME_DIAGNOSTIC_OPERATION_RESULT(ACPI_PROVIDER_OP_CLAIM, result);
	if (result.status != SYSCALL_STATUS_OK) return result.status;
	if (result.value != sizeof(response) || response.table_cap == CAP_ID_INVALID) return SYSCALL_STATUS_FAILED;
	*out_table_cap = response.table_cap;
	return SYSCALL_STATUS_OK;
}

syscall_status_t acpi_table_info(cap_id_t table_cap, struct acpi_table_info_response* out_info) {
	const struct acpi_table_info_request request = {.header = {.op = ACPI_TABLE_OP_INFO}};
	syscall_result_t                     result;

	if (table_cap == CAP_ID_INVALID || out_info == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	result = cap_call_syscall(table_cap, &request, sizeof(request), out_info, sizeof(*out_info));
	RUNTIME_DIAGNOSTIC_OPERATION_RESULT(ACPI_TABLE_OP_INFO, result);
	if (result.status != SYSCALL_STATUS_OK) return result.status;
	return result.value == sizeof(*out_info) ? SYSCALL_STATUS_OK : SYSCALL_STATUS_FAILED;
}

syscall_status_t acpi_table_read(cap_id_t table_cap, uint64_t offset, void* buffer, size_t size) {
	const struct acpi_table_read_request request = {
		.header = {.op = ACPI_TABLE_OP_READ}, .reserved = 0u, .offset = offset, .size = size};
	syscall_result_t result;

	if (table_cap == CAP_ID_INVALID || (size != 0u && buffer == NULL) || size > CAP_MAX_RESPONSE_SIZE)
		return SYSCALL_STATUS_BAD_ARGUMENT;
	result = cap_call_syscall(table_cap, &request, sizeof(request), buffer, size);
	RUNTIME_DIAGNOSTIC_OPERATION_RESULT(ACPI_TABLE_OP_READ, result);
	if (result.status != SYSCALL_STATUS_OK) return result.status;
	return result.value == size ? SYSCALL_STATUS_OK : SYSCALL_STATUS_FAILED;
}
