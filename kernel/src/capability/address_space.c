#include "address_space.h"

#include <base/address_space.h>
#include <core/address_space.h>
#include <core/capability.h>
#include <core/process.h>
#include <core/syscall.h>
#include <kernel/capability.h>
#include <string.h>

#include "memory.h"

#define PROCESS_ADDRESS_SPACE_CAP_RIGHTS                                                                               \
	((cap_rights_t)(CAP_CALL | CAP_READ | CAP_WRITE | CAP_EXEC | CAP_MAP | CAP_DELEGATE))
#define DEVICE_ADDRESS_SPACE_CAP_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_READ | CAP_WRITE | CAP_MAP | CAP_DELEGATE))

static syscall_result_t device_address_space_handler(const struct cap_request* req);

static bool access_valid(memory_access_t access) {
	return (access & ~MEMORY_ACCESS_VALID_MASK) == 0u;
}

static cap_rights_t access_rights(memory_access_t access) {
	cap_rights_t rights = 0u;
	if ((access & MEMORY_ACCESS_READ) != 0u) rights |= CAP_READ;
	if ((access & MEMORY_ACCESS_WRITE) != 0u) rights |= CAP_WRITE;
	if ((access & MEMORY_ACCESS_EXEC) != 0u) rights |= CAP_EXEC;
	return rights;
}

static syscall_result_t address_space_info_handler(const struct cap_request* req, struct address_space* space) {
	if ((req->rights & CAP_READ) == 0u) return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	const struct address_space_info response = {
		.kind                 = address_space_kind(space),
		.minimum_address      = space->base,
		.maximum_address      = space->end,
		.minimum_mapping_size = address_space_minimum_mapping_size(space),
	};
	return cap_kernel_write_response(req, &response, sizeof(response));
}

static syscall_result_t address_space_map_handler(const struct cap_request* req, struct address_space* space,
                                                  struct process* process_owner) {
	struct address_space_map_request  request;
	struct address_space_map_response response = {.mapping_cap = CAP_ID_INVALID};
	struct cap_object*                memory_object;
	struct memory*                    memory;
	cap_rights_t                      memory_rights;
	struct mapping*                   mapping;

	if (req->request_size < sizeof(request) || !cap_kernel_response_fits(req, sizeof(response)))
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	memcpy(&request, req->request, sizeof(request));
	if (!access_valid(request.access) ||
	    (address_space_kind(space) == ADDRESS_SPACE_KIND_DEVICE && (request.access & MEMORY_ACCESS_EXEC) != 0u))
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);

	cap_rights_t required = CAP_MAP | access_rights(request.access);
	if ((req->rights & required) != required) return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	syscall_result_t result =
		kernel_memory_acquire(request.memory_cap, req->caller, required, &memory_object, &memory, &memory_rights);
	if (result.status != SYSCALL_STATUS_OK) return result;

	if (!address_space_map(space,
	                       &(const struct address_space_mapping_request){
							   .memory       = memory,
							   .address      = request.address,
							   .alignment    = request.alignment,
							   .guard_before = request.guard_before,
							   .guard_after  = request.guard_after,
							   .access       = request.access,
						   },
	                       &mapping)) {
		cap_object_release(memory_object);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	cap_rights_t maximum = req->rights & memory_rights & (CAP_READ | CAP_WRITE | CAP_EXEC);
	if (address_space_kind(space) == ADDRESS_SPACE_KIND_DEVICE) maximum &= ~CAP_EXEC;
	memory_access_t maximum_access = (memory_access_t)(((maximum & CAP_READ) != 0u ? MEMORY_ACCESS_READ : 0u) |
	                                                   ((maximum & CAP_WRITE) != 0u ? MEMORY_ACCESS_WRITE : 0u) |
	                                                   ((maximum & CAP_EXEC) != 0u ? MEMORY_ACCESS_EXEC : 0u));
	cap_rights_t    mapping_rights =
		CAP_CALL | CAP_READ | CAP_MAP | CAP_DESTROY | CAP_DELEGATE | CAP_DELEGATE_PEER | maximum;

	response.mapping_cap =
		process_owner != NULL
			? kernel_mapping_publish(process_owner, req->caller, mapping, mapping_rights, maximum_access)
			: kernel_device_mapping_publish(space, req->caller, mapping, mapping_rights, maximum_access);
	response.address = mapping_address(mapping);
	if (response.mapping_cap == CAP_ID_INVALID) {
		(void)address_space_unmap(space, mapping);
		mapping_release(mapping);
		cap_object_release(memory_object);
		return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	}

	mapping_release(mapping);
	cap_object_release(memory_object);
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

	target = process_acquire((process_id_t)req->object_id);
	if (target == NULL || process_address_space(target) == NULL) {
		process_release(target);
		return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	}

	struct address_space* space = process_address_space(target);
	syscall_result_t      result;
	switch (header.op) {
	case ADDRESS_SPACE_OP_INFO:
		result = address_space_info_handler(req, space);
		break;
	case ADDRESS_SPACE_OP_MAP:
		result = address_space_map_handler(req, space, target);
		break;
	default:
		result = syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
		break;
	}
	process_release(target);
	return result;
}

static void device_address_space_destroy(uint64_t object_id) {
	address_space_device_release((struct address_space*)(uintptr_t)object_id);
}

static void device_address_space_event(struct cap_object* object, enum cap_object_event event) {
	if (event == CAP_OBJECT_EVENT_ZERO_GRANTS) (void)cap_object_destroy_if_unused(object);
}

static cap_id_t device_address_space_publish(struct address_space* space, process_id_t recipient, cap_rights_t rights) {
	cap_object_id_t object_id;
	cap_id_t        cap;
	bool            created = false;

	if (space == NULL) return CAP_ID_INVALID;
	if (recipient == PROCESS_PID_INVALID || address_space_kind(space) != ADDRESS_SPACE_KIND_DEVICE ||
	    (rights & CAP_CALL) == 0u || (rights & ~DEVICE_ADDRESS_SPACE_CAP_RIGHTS) != 0u) {
		address_space_device_release(space);
		return CAP_ID_INVALID;
	}

	object_id = cap_object_create_kernel_lifecycle((uint64_t)(uintptr_t)space,
	                                               device_address_space_handler,
	                                               NULL,
	                                               device_address_space_destroy,
	                                               device_address_space_event,
	                                               &created);
	if (object_id == CAP_OBJECT_ID_INVALID) {
		address_space_device_release(space);
		return CAP_ID_INVALID;
	}
	if (!created) address_space_device_release(space);

	cap = cap_create(object_id, recipient, rights, NULL);
	if (cap == CAP_ID_INVALID && created) (void)cap_object_destroy_with_id(object_id);
	return cap;
}

static syscall_result_t device_address_space_handler(const struct cap_request* req) {
	struct address_space_request_header header;
	struct address_space*               space;

	if (req == NULL || req->object_id == 0u || req->request == NULL || req->request_size < sizeof(header))
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	memcpy(&header, req->request, sizeof(header));
	space = (struct address_space*)(uintptr_t)req->object_id;
	if (address_space_kind(space) != ADDRESS_SPACE_KIND_DEVICE)
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);

	switch (header.op) {
	case ADDRESS_SPACE_OP_INFO:
		return address_space_info_handler(req, space);
	case ADDRESS_SPACE_OP_MAP:
		return address_space_map_handler(req, space, NULL);
	default:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
}

cap_id_t kernel_address_space_grant(struct process* process, process_id_t recipient, cap_rights_t rights) {
	struct cap_object* object;
	cap_object_id_t    object_id;
	bool               object_created = false;
	if (process == NULL || recipient == PROCESS_PID_INVALID || process_address_space(process) == NULL ||
	    (rights & CAP_CALL) == 0u || (rights & ~PROCESS_ADDRESS_SPACE_CAP_RIGHTS) != 0u)
		return CAP_ID_INVALID;
	object_id = process_address_space_cap_object_id(process);
	object    = object_id == CAP_OBJECT_ID_INVALID ? NULL : cap_object_acquire(object_id);
	if (object != NULL) cap_object_release(object);
	if (object == NULL) {
		object_id = cap_object_create_kernel((uint64_t)process_pid(process), address_space_handler, &object_created);
		if (object_id == CAP_OBJECT_ID_INVALID) return CAP_ID_INVALID;
		process_set_address_space_cap_object_id(process, object_id);
	}
	cap_id_t cap = cap_create(object_id, recipient, rights, NULL);
	if (cap == CAP_ID_INVALID && object_created) {
		process_set_address_space_cap_object_id(process, CAP_OBJECT_ID_INVALID);
		(void)cap_object_destroy_with_id(object_id);
	}
	return cap;
}

cap_id_t kernel_device_address_space_grant(struct address_space* space, process_id_t recipient, cap_rights_t rights) {
	if (!address_space_device_retain(space)) return CAP_ID_INVALID;
	return device_address_space_publish(space, recipient, rights);
}

syscall_result_t kernel_device_address_space_acquire(cap_id_t address_space_cap, process_id_t caller,
                                                     cap_rights_t required_rights, struct cap_object** out_object,
                                                     struct address_space** out_space, cap_rights_t* out_rights) {
	struct cap_object* object;
	cap_rights_t       rights;

	if (out_object == NULL || out_space == NULL || address_space_cap == CAP_ID_INVALID ||
	    caller == PROCESS_PID_INVALID) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	*out_object = NULL;
	*out_space  = NULL;

	enum cap_result result = cap_object_acquire_for_use(caller, address_space_cap, required_rights, &object, &rights);
	if (result != CAP_OK) {
		return syscall_result_error(result == CAP_NOT_AUTHORIZED || result == CAP_RIGHTS_EXCEEDED
		                                ? SYSCALL_STATUS_DENIED
		                                : SYSCALL_STATUS_BAD_ARGUMENT,
		                            0u);
	}
	if (object == NULL || object->handler != device_address_space_handler || object->object_id == 0u) {
		cap_object_release(object);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	struct address_space* space = (struct address_space*)(uintptr_t)object->object_id;
	if (address_space_kind(space) != ADDRESS_SPACE_KIND_DEVICE) {
		cap_object_release(object);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	*out_object = object;
	*out_space  = space;
	if (out_rights != NULL) *out_rights = rights;
	return syscall_result_ok(0u);
}
