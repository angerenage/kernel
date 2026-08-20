#include "serial.h"

#include <base/cap.h>
#include <base/syscall.h>
#include <core/capability.h>
#include <hal/serial.h>
#include <kernel/capability.h>
#include <stddef.h>
#include <stdint.h>

static cap_object_id_t serial_object_id = CAP_OBJECT_ID_INVALID;

static syscall_result_t serial_handler(const struct cap_request* req) {
	if ((req->rights & CAP_WRITE) != CAP_WRITE) {
		return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	}
	if (req->request_size == 0u || req->request == NULL) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	if (req->request_size > CAP_MAX_REQUEST_SIZE) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	hal_serial_write((const char*)req->request, req->request_size);
	return syscall_result_ok(0u);
}

void kernel_capability_serial_init(void) {
	serial_object_id = cap_object_create_kernel(0u, serial_handler, NULL);
}

cap_id_t kernel_capability_serial_grant(process_id_t target) {
	if (serial_object_id == CAP_OBJECT_ID_INVALID) return CAP_ID_INVALID;
	return cap_create(serial_object_id, target, CAP_WRITE | CAP_CALL | CAP_DELEGATE | CAP_DELEGATE_PEER, NULL);
}
