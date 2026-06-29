#include <system/capability.h>

#include "syscall.h"

syscall_result_t cap_call_syscall(cap_id_t cap, const void* request, size_t request_size, void* response,
                                  size_t response_capacity) {
	return syscall(SYSCALL_CAP_CALL,
	               (uintptr_t)cap,
	               (uintptr_t)request,
	               (uintptr_t)request_size,
	               (uintptr_t)response,
	               (uintptr_t)response_capacity,
	               0u);
}

bool cap_publish(channel_id_t endpoint_id, uint64_t object_id, process_id_t target, cap_rights_t rights,
                 cap_id_t* out) {
	cap_id_t         cap_id = CAP_ID_INVALID;
	syscall_result_t result = syscall(SYSCALL_CAP_CREATE,
	                                  (uintptr_t)endpoint_id,
	                                  (uintptr_t)target,
	                                  (uintptr_t)object_id,
	                                  (uintptr_t)rights,
	                                  (uintptr_t)&cap_id,
	                                  0u);

	if (result.status != SYSCALL_STATUS_OK) return false;
	if (out != NULL) *out = cap_id;
	return true;
}

bool cap_delegate(cap_id_t source, process_id_t target, cap_rights_t rights, cap_id_t* out) {
	cap_id_t         cap_id = CAP_ID_INVALID;
	syscall_result_t result = syscall(
		SYSCALL_CAP_DELEGATE, (uintptr_t)source, (uintptr_t)target, (uintptr_t)rights, (uintptr_t)&cap_id, 0u, 0u);

	if (result.status != SYSCALL_STATUS_OK) return false;
	if (out != NULL) *out = cap_id;
	return true;
}

bool cap_derive(cap_id_t base, process_id_t target, uint64_t object_id, cap_rights_t rights, cap_id_t* out) {
	cap_id_t         cap_id = CAP_ID_INVALID;
	syscall_result_t result = syscall(SYSCALL_CAP_DERIVE,
	                                  (uintptr_t)base,
	                                  (uintptr_t)target,
	                                  (uintptr_t)object_id,
	                                  (uintptr_t)rights,
	                                  (uintptr_t)&cap_id,
	                                  0u);

	if (result.status != SYSCALL_STATUS_OK) return false;
	if (out != NULL) *out = cap_id;
	return true;
}

bool cap_revoke(cap_id_t cap, cap_rights_t rights) {
	syscall_result_t result = syscall(SYSCALL_CAP_REVOKE, (uintptr_t)cap, (uintptr_t)rights, 0u, 0u, 0u, 0u);
	return result.status == SYSCALL_STATUS_OK;
}

bool cap_call(cap_id_t cap, const void* request, size_t request_size, void* response, size_t response_capacity,
              size_t* result_value) {
	syscall_result_t result;

	if (request == NULL || request_size == 0u) return false;
	if (response_capacity != 0u && response == NULL) return false;
	result = cap_call_syscall(cap, request, request_size, response, response_capacity);
	if (result.status != SYSCALL_STATUS_OK) return false;
	if (result_value != NULL) *result_value = (size_t)result.value;
	return true;
}
