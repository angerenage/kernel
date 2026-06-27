#include <base/cap.h>
#include <base/syscall.h>
#include <core/capability.h>
#include <hal/serial.h>
#include <kernel/capability.h>
#include <stddef.h>
#include <stdint.h>

static struct cap_object* serial_object;

static syscall_result_t serial_handler(const struct cap_request* req) {
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
	serial_object            = cap_object_create_kernel(0u, serial_handler);
	serial_object->object_id = (uint64_t)(uintptr_t)serial_object; // Use the pointer as a unique ID for the object.
}

cap_id_t kernel_capability_serial_grant(process_id_t target) {
	struct capability* cap;

	if (serial_object == NULL) return CAP_ID_INVALID;

	cap = cap_create(serial_object->cap_object_id, target, CAP_WRITE | CAP_CALL, NULL);
	if (cap == NULL) return CAP_ID_INVALID;

	return cap->cap_id;
}
