#include <base/cap.h>
#include <base/syscall.h>
#include <core/address_transfer.h>
#include <core/capability.h>
#include <core/process.h>
#include <hal/serial.h>
#include <kernel/capability.h>
#include <libc/stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static struct cap_object* serial_object;

static syscall_result_t serial_handler(const struct cap_request* req) {
	struct address_space*        space;
	enum address_transfer_result result;
	size_t                       size;
	uint8_t*                     buffer;

	if (req->request_size == 0u || req->request == NULL) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	size = req->request_size;
	if (size > CAP_MAX_REQUEST_SIZE) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	buffer = malloc(size);
	if (buffer == NULL) return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);

	{
		struct process* process = process_lookup(req->caller);
		if (process == NULL) {
			free(buffer);
			return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
		}
		space = process_address_space(process);
	}

	result = address_space_copy_from(space, (uintptr_t)req->request, buffer, size);
	if (result != ADDRESS_TRANSFER_OK) {
		free(buffer);
		if (result == ADDRESS_TRANSFER_FAULT_FAILED) {
			return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
		}
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	hal_serial_write((const char*)buffer, size);
	free(buffer);
	return syscall_result_ok((uintptr_t)size);
}

void kernel_capability_serial_init(void) {
	serial_object            = cap_object_create_kernel(0u, serial_handler);
	serial_object->object_id = (uint64_t)(uintptr_t)serial_object; // Use the pointer as a unique ID for the object.
}

cap_id_t kernel_capability_serial_grant(process_id_t target) {
	struct capability* cap;

	if (serial_object == NULL) return CAP_ID_INVALID;

	cap = cap_create(serial_object, target, CAP_WRITE | CAP_CALL, NULL);
	if (cap == NULL) return CAP_ID_INVALID;

	return cap->cap_id;
}
