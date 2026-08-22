#include <runtime/diagnostic.h>
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

syscall_status_t cap_publish(channel_id_t endpoint_id, uint64_t object_id, process_id_t target, cap_rights_t rights,
                             cap_id_t* out) {
	cap_id_t         cap_id = CAP_ID_INVALID;
	syscall_result_t result = syscall(SYSCALL_CAP_CREATE,
	                                  (uintptr_t)endpoint_id,
	                                  (uintptr_t)target,
	                                  (uintptr_t)object_id,
	                                  (uintptr_t)rights,
	                                  (uintptr_t)&cap_id,
	                                  0u);

#ifdef RUNTIME_DIAGNOSTICS
	if (result.status == SYSCALL_STATUS_BAD_ARGUMENT) {
		switch (result.value) {
		case 0u:
			RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(endpoint_id);
			break;
		case 1u:
			RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(target);
			break;
		case 2u:
			RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(object_id);
			break;
		case 3u:
			RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(rights);
			break;
		default:
			RUNTIME_DIAGNOSTIC_INVALID_PARAMETER_INDEX(SYSCALL_CAP_CREATE, result.value);
			break;
		}
	}
	else {
		RUNTIME_DIAGNOSTIC_SYSCALL_RESULT(SYSCALL_CAP_CREATE, result);
	}
#endif

	if (result.status == SYSCALL_STATUS_OK && out != NULL) *out = cap_id;
	return result.status;
}

static syscall_status_t cap_delegate_with_flags(cap_id_t source, process_id_t target, cap_rights_t rights,
                                                enum cap_delegate_flag flags, cap_id_t* out) {
	cap_id_t         cap_id = CAP_ID_INVALID;
	syscall_result_t result = syscall(SYSCALL_CAP_DELEGATE,
	                                  (uintptr_t)source,
	                                  (uintptr_t)target,
	                                  (uintptr_t)rights,
	                                  (uintptr_t)&cap_id,
	                                  (uintptr_t)flags,
	                                  0u);

#ifdef RUNTIME_DIAGNOSTICS
	if (result.status == SYSCALL_STATUS_BAD_ARGUMENT) {
		switch (result.value) {
		case 0u:
			RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(source);
			break;
		case 1u:
			RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(target);
			break;
		case 2u:
			RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(rights);
			break;
		case 4u:
			RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(flags);
			break;
		default:
			RUNTIME_DIAGNOSTIC_INVALID_PARAMETER_INDEX(SYSCALL_CAP_DELEGATE, result.value);
			break;
		}
	}
	else {
		RUNTIME_DIAGNOSTIC_SYSCALL_RESULT(SYSCALL_CAP_DELEGATE, result);
	}
#endif

	if (result.status == SYSCALL_STATUS_OK && out != NULL) *out = cap_id;
	return result.status;
}

syscall_status_t cap_delegate(cap_id_t source, process_id_t target, cap_rights_t rights, cap_id_t* out) {
	return cap_delegate_with_flags(source, target, rights, CAP_DELEGATE_FLAG_NONE, out);
}

syscall_status_t cap_delegate_peer(cap_id_t source, process_id_t target, cap_rights_t rights, cap_id_t* out) {
	return cap_delegate_with_flags(source, target, rights, CAP_DELEGATE_FLAG_PEER, out);
}

syscall_status_t cap_derive(cap_id_t base, process_id_t target, uint64_t object_id, cap_rights_t rights,
                            cap_id_t* out) {
	cap_id_t         cap_id = CAP_ID_INVALID;
	syscall_result_t result = syscall(SYSCALL_CAP_DERIVE,
	                                  (uintptr_t)base,
	                                  (uintptr_t)target,
	                                  (uintptr_t)object_id,
	                                  (uintptr_t)rights,
	                                  (uintptr_t)&cap_id,
	                                  0u);

#ifdef RUNTIME_DIAGNOSTICS
	if (result.status == SYSCALL_STATUS_BAD_ARGUMENT) {
		switch (result.value) {
		case 0u:
			RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(base);
			break;
		case 1u:
			RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(target);
			break;
		case 2u:
			RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(object_id);
			break;
		case 3u:
			RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(rights);
			break;
		default:
			RUNTIME_DIAGNOSTIC_INVALID_PARAMETER_INDEX(SYSCALL_CAP_DERIVE, result.value);
			break;
		}
	}
	else {
		RUNTIME_DIAGNOSTIC_SYSCALL_RESULT(SYSCALL_CAP_DERIVE, result);
	}
#endif

	if (result.status == SYSCALL_STATUS_OK && out != NULL) *out = cap_id;
	return result.status;
}

syscall_status_t cap_revoke(cap_id_t cap, cap_rights_t rights) {
	syscall_result_t result = syscall(SYSCALL_CAP_REVOKE, (uintptr_t)cap, (uintptr_t)rights, 0u, 0u, 0u, 0u);

#ifdef RUNTIME_DIAGNOSTICS
	if (result.status == SYSCALL_STATUS_BAD_ARGUMENT) {
		if (result.value == 0u) {
			RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(cap);
		}
		else if (result.value == 1u) {
			RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(rights);
		}
		else {
			RUNTIME_DIAGNOSTIC_INVALID_PARAMETER_INDEX(SYSCALL_CAP_REVOKE, result.value);
		}
	}
	else {
		RUNTIME_DIAGNOSTIC_SYSCALL_RESULT(SYSCALL_CAP_REVOKE, result);
	}
#endif

	return result.status;
}

syscall_status_t cap_unpublish(channel_id_t endpoint, uint64_t object_id) {
	syscall_result_t result = syscall(SYSCALL_CAP_UNPUBLISH, (uintptr_t)endpoint, (uintptr_t)object_id, 0u, 0u, 0u, 0u);
	return result.status;
}

syscall_status_t cap_drop(cap_id_t cap) {
	syscall_result_t result = syscall(SYSCALL_CAP_DROP, (uintptr_t)cap, 0u, 0u, 0u, 0u, 0u);

#ifdef RUNTIME_DIAGNOSTICS
	if (result.status == SYSCALL_STATUS_BAD_ARGUMENT) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(cap);
	}
	else {
		RUNTIME_DIAGNOSTIC_SYSCALL_RESULT(SYSCALL_CAP_DROP, result);
	}
#endif

	return result.status;
}

syscall_status_t cap_valid(cap_id_t cap, bool* out_valid) {
	syscall_result_t result;

	if (out_valid == NULL) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(out_valid);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	*out_valid = false;

	result = syscall(SYSCALL_CAP_VALID, (uintptr_t)cap, 0u, 0u, 0u, 0u, 0u);
	RUNTIME_DIAGNOSTIC_SYSCALL_RESULT(SYSCALL_CAP_VALID, result);
	if (result.status == SYSCALL_STATUS_OK) *out_valid = result.value != 0u;
	return result.status;
}

syscall_status_t cap_call(cap_id_t cap, const void* request, size_t request_size, void* response,
                          size_t response_capacity, size_t* result_value) {
	syscall_result_t result;

	if (cap == CAP_ID_INVALID) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(cap);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if (request == NULL) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(request);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if (request_size == 0u) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(request_size);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if (request_size > CAP_MAX_REQUEST_SIZE) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(request_size);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if (response_capacity != 0u && response == NULL) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(response);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if (response_capacity > CAP_MAX_RESPONSE_SIZE) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(response_capacity);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}

	result = cap_call_syscall(cap, request, request_size, response, response_capacity);
	RUNTIME_DIAGNOSTIC_OPERATION_RESULT(cap_call, result);
	if (result.status == SYSCALL_STATUS_OK && result_value != NULL) *result_value = (size_t)result.value;
	return result.status;
}
