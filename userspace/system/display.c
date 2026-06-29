#include <base/cap.h>
#include <base/display.h>
#include <base/syscall.h>
#include <stddef.h>
#include <stdint.h>

#include "syscall.h"

cap_id_t serial_cap_id = CAP_ID_INVALID;

bool display_write(const char* data, size_t length) {
	syscall_result_t result;

	if (serial_cap_id == CAP_ID_INVALID) return false;
	if (length != 0u && data == NULL) return false;
	result = syscall(SYSCALL_CAP_CALL, (uintptr_t)serial_cap_id, (uintptr_t)data, (uintptr_t)length, 0u, 0u, 0u);
	return result.status == SYSCALL_STATUS_OK;
}
