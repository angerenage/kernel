#include <base/cap.h>
#include <core/capability.h>
#include <kernel/capability.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../capability/boot_module.h"
#include "../capability/loader.h"
#include "../capability/serial.h"

cap_id_t cap_kernel_create(uint64_t object_id, cap_kernel_handler_t handler, process_id_t target, cap_rights_t rights) {
	cap_object_id_t cap_object_id;
	cap_id_t        cap_id;
	bool            object_created = false;

	cap_object_id = cap_object_create_kernel(object_id, handler, &object_created);
	if (cap_object_id == CAP_OBJECT_ID_INVALID) return CAP_ID_INVALID;

	cap_id = cap_create(cap_object_id, target, rights, NULL, NULL);
	if (cap_id == CAP_ID_INVALID) {
		if (object_created) (void)cap_object_destroy_with_id(cap_object_id);
		return CAP_ID_INVALID;
	}

	return cap_id;
}

bool cap_kernel_response_fits(const struct cap_request* request, size_t response_size) {
	return request != NULL && response_size != 0u && request->response != NULL &&
	       response_size <= request->response_capacity;
}

syscall_result_t cap_kernel_write_response(const struct cap_request* request, const void* response,
                                           size_t response_size) {
	if (response == NULL || !cap_kernel_response_fits(request, response_size)) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	memcpy(request->response, response, response_size);
	return syscall_result_ok(response_size);
}

void kernel_capability_init(void) {
	kernel_capability_serial_init();
	kernel_capability_loader_init();
}
