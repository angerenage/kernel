#include <base/record.h>
#include <runtime/diagnostic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <system/capability.h>
#include <system/record.h>

syscall_status_t record_count(cap_id_t record_cap, uint64_t* out_count) {
	const struct record_count_request request = {.header = {.op = RECORD_OP_COUNT}};
	struct record_count_response      response;
	syscall_result_t                  result;

	if (record_cap == CAP_ID_INVALID || out_count == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	*out_count = 0u;
	result     = cap_call_syscall(record_cap, &request, sizeof(request), &response, sizeof(response));
	RUNTIME_DIAGNOSTIC_OPERATION_RESULT(RECORD_OP_COUNT, result);
	if (result.status != SYSCALL_STATUS_OK) return result.status;
	if (result.value != sizeof(response)) return SYSCALL_STATUS_FAILED;
	*out_count = response.count;
	return SYSCALL_STATUS_OK;
}

syscall_status_t record_get(cap_id_t record_cap, uint64_t index, void* out_record, size_t record_size) {
	const struct record_get_request request = {.header = {.op = RECORD_OP_GET}, .index = index};
	syscall_result_t                result;

	if (record_cap == CAP_ID_INVALID || out_record == NULL || record_size == 0u ||
	    record_size > CAP_MAX_RESPONSE_SIZE) {
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	memset(out_record, 0, record_size);
	result = cap_call_syscall(record_cap, &request, sizeof(request), out_record, record_size);
	RUNTIME_DIAGNOSTIC_OPERATION_RESULT(RECORD_OP_GET, result);
	if (result.status != SYSCALL_STATUS_OK) return result.status;
	if (result.value != record_size) {
		memset(out_record, 0, record_size);
		return SYSCALL_STATUS_FAILED;
	}
	return SYSCALL_STATUS_OK;
}
