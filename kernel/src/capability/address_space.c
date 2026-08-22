#include "address_space.h"

#include <base/address_space.h>
#include <base/cap.h>
#include <core/capability.h>
#include <core/process.h>
#include <core/syscall.h>
#include <kernel/capability.h>
#include <string.h>

#include "memory.h"

static syscall_result_t address_space_map_handler(const struct cap_request* req, struct process* target) {
	struct address_space_map_request request;
	struct address_space_map_result  response;
	if (req->request == NULL || req->request_size < sizeof(request) || !cap_kernel_response_fits(req, sizeof(response)))
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	memcpy(&request, req->request, sizeof(request));
	syscall_result_t result = kernel_memory_map(request.memory_cap, req->caller, target, &request.params, &response);
	if (result.status != SYSCALL_STATUS_OK) return result;
	result = cap_kernel_write_response(req, &response, sizeof(response));
	if (result.status != SYSCALL_STATUS_OK) (void)kernel_mapping_discard_unpublished(response.mapping_cap, req->caller);
	return result;
}

static syscall_result_t address_space_handler(const struct cap_request* req) {
	struct address_space_request_header header;
	struct process*                     target;
	if (req == NULL || req->request == NULL || req->request_size < sizeof(header))
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	memcpy(&header, req->request, sizeof(header));
	if (header.op != ADDRESS_SPACE_OP_MAP) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	if ((req->rights & CAP_MAP) == 0u) return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	target = process_acquire((process_id_t)req->object_id);
	if (target == NULL || process_address_space(target) == NULL) {
		process_release(target);
		return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	}
	syscall_result_t result = address_space_map_handler(req, target);
	process_release(target);
	return result;
}

cap_id_t kernel_address_space_grant(struct process* process, process_id_t recipient, cap_rights_t rights) {
	struct cap_object* object;
	cap_object_id_t    object_id;
	bool               object_created = false;

	if (process == NULL || recipient == PROCESS_PID_INVALID || process_address_space(process) == NULL)
		return CAP_ID_INVALID;
	object_id = process_address_space_cap_object_id(process);
	object    = object_id == CAP_OBJECT_ID_INVALID ? NULL : cap_object_acquire(object_id);
	if (object != NULL) cap_object_release(object);
	if (object == NULL) {
		object_id = cap_object_create_kernel((uint64_t)process_pid(process), address_space_handler, &object_created);
		if (object_id == CAP_OBJECT_ID_INVALID) return CAP_ID_INVALID;
		process_set_address_space_cap_object_id(process, object_id);
	}
	cap_id_t cap_id = cap_create(object_id, recipient, rights, NULL);
	if (cap_id == CAP_ID_INVALID && object_created) {
		process_set_address_space_cap_object_id(process, CAP_OBJECT_ID_INVALID);
		(void)cap_object_destroy_with_id(object_id);
	}
	return cap_id;
}
