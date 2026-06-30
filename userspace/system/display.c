#include <base/cap.h>
#include <base/display.h>
#include <base/syscall.h>
#include <stddef.h>
#include <stdint.h>

#include "syscall.h"

cap_id_t serial_cap_id = CAP_ID_INVALID;

syscall_status_t display_write(const char* data, size_t length) {
	syscall_result_t result;

	if (serial_cap_id == CAP_ID_INVALID) return SYSCALL_STATUS_FAILED;
	if (length != 0u && data == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	result = syscall(SYSCALL_CAP_CALL, (uintptr_t)serial_cap_id, (uintptr_t)data, (uintptr_t)length, 0u, 0u, 0u);
	return result.status;
}
