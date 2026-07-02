#include "address_space.h"

#include <base/address_space.h>
#include <base/cap.h>
#include <core/capability.h>
#include <core/process.h>
#include <core/syscall.h>
#include <core/vmm.h>
#include <kernel/capability.h>
#include <string.h>

#include "memory.h"

static syscall_result_t copy_request(const void* req_ptr, size_t req_size, void* out_buf, size_t expected_size) {
	if (req_ptr == NULL || out_buf == NULL || req_size < expected_size) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	memcpy(out_buf, req_ptr, expected_size);
	return syscall_result_ok(0u);
}

static syscall_result_t address_space_query_handler(const struct cap_request* req, struct address_space* space) {
	struct address_space_query_request  request;
	struct address_space_query_response response;
	syscall_result_t                    copy_result;

	copy_result = copy_request(req->request, req->request_size, &request, sizeof(request));
	if (copy_result.status != SYSCALL_STATUS_OK) return copy_result;
	if (request.id == VMM_ID_INVALID || !vmm_query_id(space, request.id, &response.info)) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	return cap_kernel_write_response(req, &response, sizeof(response));
}

static syscall_result_t address_space_map_handler(const struct cap_request* req, struct process* target,
                                                  bool at_explicit) {
	union {
		struct address_space_map_request    automatic;
		struct address_space_map_at_request explicit;
	} request;
	struct address_space_map_response response;
	cap_id_t                          allocation_cap;
	uintptr_t                         address = 0u;
	syscall_result_t                  result;

	if (!cap_kernel_response_fits(req, sizeof(response))) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	if (at_explicit) {
		result = copy_request(req->request, req->request_size, &request.explicit, sizeof(request.explicit));
		if (result.status != SYSCALL_STATUS_OK) return result;
		allocation_cap = request.explicit.allocation_cap;
		address        = request.explicit.address;
	}
	else {
		result = copy_request(req->request, req->request_size, &request.automatic, sizeof(request.automatic));
		if (result.status != SYSCALL_STATUS_OK) return result;
		allocation_cap = request.automatic.allocation_cap;
	}

	result = kernel_map_allocation(allocation_cap, req->caller, target, address, &response.mapping_cap);
	if (result.status != SYSCALL_STATUS_OK) return result;
	result = cap_kernel_write_response(req, &response, sizeof(response));
	if (result.status != SYSCALL_STATUS_OK) {
		(void)kernel_mapping_discard_unpublished(response.mapping_cap, req->caller);
	}
	return result;
}

static syscall_result_t address_space_handler(const struct cap_request* req) {
	struct process*       target;
	struct address_space* space;
	enum address_space_op op;
	cap_rights_t          required_rights;

	target = (struct process*)(uintptr_t)req->object_id;
	if (target == NULL || process_lookup(process_pid(target)) != target) {
		return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	}
	space = process_address_space(target);
	if (space == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);

	if (req->request_size < sizeof(struct address_space_request_header)) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	struct address_space_request_header header;
	syscall_result_t copy_result = copy_request(req->request, req->request_size, &header, sizeof(header));
	if (copy_result.status != SYSCALL_STATUS_OK) return copy_result;

	op = header.op;

	switch (op) {
	case ADDRESS_SPACE_OP_QUERY:
		required_rights = CAP_READ;
		break;
	case ADDRESS_SPACE_OP_MAP:
	case ADDRESS_SPACE_OP_MAP_AT:
		required_rights = CAP_MAP;
		break;
	default:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	if ((req->rights & required_rights) != required_rights) {
		return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	}

	switch (op) {
	case ADDRESS_SPACE_OP_QUERY:
		return address_space_query_handler(req, space);
	case ADDRESS_SPACE_OP_MAP:
		return address_space_map_handler(req, target, false);
	case ADDRESS_SPACE_OP_MAP_AT:
		return address_space_map_handler(req, target, true);
	default:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
}

cap_id_t kernel_address_space_grant(struct process* process, process_id_t recipient, cap_rights_t rights) {
	struct cap_object* object;
	cap_object_id_t    object_id;
	struct capability* cap;

	if (process == NULL || recipient == PROCESS_PID_INVALID || process_address_space(process) == NULL) {
		return CAP_ID_INVALID;
	}

	object_id = process_address_space_cap_object_id(process);
	if (object_id != CAP_OBJECT_ID_INVALID) {
		object = cap_object_acquire(object_id);
		if (object != NULL) cap_object_release(object);
	}
	else {
		object = NULL;
	}
	if (object == NULL) {
		object = cap_object_create_kernel((uint64_t)(uintptr_t)process, address_space_handler);
		if (object == NULL) return CAP_ID_INVALID;
		object_id = object->cap_object_id;
		process_set_address_space_cap_object_id(process, object_id);
	}

	cap = cap_create(object_id, recipient, rights, NULL);
	if (cap == NULL) return CAP_ID_INVALID;

	return cap->cap_id;
}
