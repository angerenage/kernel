#include <base/cap.h>
#include <base/display.h>
#include <base/syscall.h>
#include <runtime/diagnostic.h>
#include <stddef.h>
#include <stdint.h>

#include "syscall.h"

cap_id_t serial_cap_id = CAP_ID_INVALID;

syscall_status_t display_write(const char* data, size_t length) {
	syscall_result_t result;
	size_t           offset = 0u;

	if (serial_cap_id == CAP_ID_INVALID) {
		RUNTIME_DIAGNOSTIC_INVALID_STATE("serial capability is unavailable");
		return SYSCALL_STATUS_FAILED;
	}
	if (length != 0u && data == NULL) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(data);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}

	do {
		size_t chunk = length - offset;
		if (chunk > CAP_MAX_REQUEST_SIZE) chunk = CAP_MAX_REQUEST_SIZE;

		result =
			syscall(SYSCALL_CAP_CALL, (uintptr_t)serial_cap_id, (uintptr_t)data + offset, (uintptr_t)chunk, 0u, 0u, 0u);
		if (result.status != SYSCALL_STATUS_OK) break;
		offset += chunk;
	} while (offset < length);

#ifdef RUNTIME_DIAGNOSTICS
	if (result.status == SYSCALL_STATUS_BAD_ARGUMENT) {
		if (result.value == 1u) {
			RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(data);
		}
		else if (result.value == 2u) {
			RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(length);
		}
		else {
			RUNTIME_DIAGNOSTIC_INVALID_PARAMETER_INDEX(display_write, result.value);
		}
	}
	else {
		RUNTIME_DIAGNOSTIC_SYSCALL_RESULT(display_write, result);
	}
#endif

	return result.status;
}
