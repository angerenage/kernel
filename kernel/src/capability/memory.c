#include "memory.h"

#include <base/cap.h>
#include <base/math.h>
#include <base/memory.h>
#include <base/vmm.h>
#include <core/address_transfer.h>
#include <core/capability.h>
#include <core/memory_object.h>
#include <core/mm.h>
#include <core/pmm.h>
#include <core/process.h>
#include <core/syscall.h>
#include <core/vm_space.h>
#include <hal/paging.h>
#include <kernel/capability.h>
#include <stdlib.h>
#include <string.h>

/* Immutable identity of a mapping controlled by a mapping capability. */
struct mapping_state {
	process_id_t space_owner;
	vmm_id_t     mapping_id;
};

static syscall_result_t memory_handler(const struct cap_request* req);
static syscall_result_t mapping_handler(const struct cap_request* req);

static syscall_result_t copy_request(const void* request, size_t request_size, void* out, size_t expected_size) {
	if (request == NULL || out == NULL || request_size < expected_size)
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	memcpy(out, request, expected_size);
	return syscall_result_ok(0u);
}

static bool user_prot_is_valid(vmm_prot_t prot) {
	return (prot & ~(VMM_PROT_READ | VMM_PROT_WRITE | VMM_PROT_EXEC)) == 0u;
}

static cap_rights_t protection_rights(vmm_prot_t prot) {
	cap_rights_t rights = 0u;
	if ((prot & VMM_PROT_READ) != 0u) rights |= CAP_READ;
	if ((prot & VMM_PROT_WRITE) != 0u) rights |= CAP_WRITE;
	if ((prot & VMM_PROT_EXEC) != 0u) rights |= CAP_EXEC;
	return rights;
}

static void memory_destroy(uint64_t object_id) {
	memory_object_release((struct memory_object*)(uintptr_t)object_id);
}

static void memory_event(struct cap_object* object, enum cap_object_event event) {
	if (event == CAP_OBJECT_EVENT_ZERO_GRANTS) (void)cap_object_destroy_if_unused(object);
}

static void mapping_destroy(uint64_t object_id) {
	free((struct mapping_state*)(uintptr_t)object_id);
}

static void mapping_event(struct cap_object* object, enum cap_object_event event) {
	if (event == CAP_OBJECT_EVENT_ZERO_GRANTS) (void)cap_object_destroy_if_unused(object);
}

static bool mapping_process_cleanup(uint64_t object_id, process_id_t process) {
	const struct mapping_state* state = (const struct mapping_state*)(uintptr_t)object_id;
	return state != NULL && state->space_owner == process;
}

static cap_id_t mapping_create(process_id_t target, process_id_t space_owner, vmm_id_t mapping_id,
                               cap_rights_t rights) {
	struct mapping_state* state;
	cap_object_id_t       object_id;
	cap_id_t              cap_id;

	if (target == PROCESS_PID_INVALID || space_owner == PROCESS_PID_INVALID || mapping_id == VMM_ID_INVALID ||
	    (rights & ~(CAP_READ | CAP_WRITE | CAP_EXEC | CAP_MAP | CAP_DESTROY | CAP_DELEGATE)) != 0u)
		return CAP_ID_INVALID;
	state = malloc(sizeof(*state));
	if (state == NULL) return CAP_ID_INVALID;
	state->space_owner = space_owner;
	state->mapping_id  = mapping_id;
	object_id          = cap_object_create_kernel_lifecycle(
        (uint64_t)(uintptr_t)state, mapping_handler, mapping_process_cleanup, mapping_destroy, mapping_event, NULL);
	if (object_id == CAP_OBJECT_ID_INVALID) {
		free(state);
		return CAP_ID_INVALID;
	}
	cap_id = cap_create(object_id, target, CAP_CALL | rights, NULL);
	if (cap_id == CAP_ID_INVALID) (void)cap_object_destroy_with_id(object_id);
	return cap_id;
}

cap_id_t kernel_memory_create(cap_rights_t rights, size_t page_count) {
	struct memory_object* memory;
	struct process*       caller;
	cap_object_id_t       object_id;
	cap_id_t              cap_id;

	if (page_count == 0u || page_count > SIZE_MAX / PMM_PAGE_SIZE) return CAP_ID_INVALID;
	caller = process_current();
	if (caller == NULL || !memory_object_create_owned(page_count, &memory)) return CAP_ID_INVALID;
	object_id = cap_object_create_kernel_lifecycle(
		(uint64_t)(uintptr_t)memory, memory_handler, NULL, memory_destroy, memory_event, NULL);
	if (object_id == CAP_OBJECT_ID_INVALID) {
		memory_object_release(memory);
		return CAP_ID_INVALID;
	}
	cap_id = cap_create(object_id, process_pid(caller), rights, NULL);
	if (cap_id == CAP_ID_INVALID) (void)cap_object_destroy_with_id(object_id);
	return cap_id;
}

static bool map_params_are_valid(const struct memory_map_params* params) {
	size_t align_pages;
	size_t ignored;
	if (params == NULL || params->page_count == 0u || !user_prot_is_valid(params->prot) ||
	    params->memory_type >= MEMORY_TYPE_COUNT || (params->address & (PMM_PAGE_SIZE - 1u)) != 0u)
		return false;
	align_pages = params->align_pages == 0u ? 1u : params->align_pages;
	return (align_pages & (align_pages - 1u)) == 0u && !mul_overflow_size(align_pages, PMM_PAGE_SIZE, &ignored) &&
	       !add_overflow_size(params->memory_page_offset, params->page_count, &ignored) &&
	       !add_overflow_size(params->guard_pages, params->page_count, &ignored);
}

syscall_result_t kernel_memory_map(cap_id_t memory_cap_id, process_id_t caller, struct process* target,
                                   const struct memory_map_params*  params,
                                   struct address_space_map_result* out_result) {
	struct cap_object*    object;
	struct memory_object* memory;
	struct process*       retained_target;
	struct address_space* space;
	cap_rights_t          memory_rights;
	cap_rights_t          required_rights;
	vmm_id_t              mapping_id = VMM_ID_INVALID;
	void*                 base       = NULL;
	cap_id_t              mapping_cap;

	if (memory_cap_id == CAP_ID_INVALID || caller == PROCESS_PID_INVALID || target == NULL || out_result == NULL ||
	    !map_params_are_valid(params))
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	*out_result     = (struct address_space_map_result){.mapping_cap = CAP_ID_INVALID};
	required_rights = CAP_MAP | protection_rights(params->prot);
	enum cap_result cap_result =
		cap_object_acquire_for_use(caller, memory_cap_id, required_rights, &object, &memory_rights);
	if (cap_result != CAP_OK)
		return syscall_result_error(cap_result == CAP_NOT_AUTHORIZED || cap_result == CAP_RIGHTS_EXCEEDED
		                                ? SYSCALL_STATUS_DENIED
		                                : SYSCALL_STATUS_BAD_ARGUMENT,
		                            0u);
	if (object == NULL || object->handler != memory_handler) {
		cap_object_release(object);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	memory          = (struct memory_object*)(uintptr_t)object->object_id;
	retained_target = process_acquire(process_pid(target));
	if (memory == NULL || retained_target == NULL || retained_target != target) {
		process_release(retained_target);
		cap_object_release(object);
		return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	}
	space                               = process_address_space(retained_target);
	const struct vm_map_request request = {
		.memory             = memory,
		.memory_page_offset = params->memory_page_offset,
		.page_count         = params->page_count,
		.requested_base     = params->address,
		.align_pages        = params->align_pages,
		.guard_pages        = params->guard_pages,
		.prot               = params->prot,
		.memory_type        = params->memory_type,
	};
	if (!vm_space_map(space, &request, &mapping_id, &base)) {
		process_release(retained_target);
		cap_object_release(object);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	mapping_cap =
		mapping_create(caller,
	                   process_pid(retained_target),
	                   mapping_id,
	                   CAP_MAP | CAP_DESTROY | CAP_DELEGATE | (memory_rights & (CAP_READ | CAP_WRITE | CAP_EXEC)));
	if (mapping_cap == CAP_ID_INVALID) {
		(void)vm_space_unmap(space, mapping_id);
		process_release(retained_target);
		cap_object_release(object);
		return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	}
	out_result->mapping_cap = mapping_cap;
	out_result->mapping     = (struct vmm_info){
			.id                 = VMM_ID_INVALID,
			.base               = base,
			.page_count         = params->page_count,
			.memory_page_offset = params->memory_page_offset,
			.guard_pages        = params->guard_pages,
			.prot               = params->prot,
			.memory_type        = params->memory_type,
    };
	process_release(retained_target);
	cap_object_release(object);
	return syscall_result_ok(0u);
}

cap_id_t kernel_mapping_grant(struct process* target, process_id_t recipient, vmm_id_t mapping_id,
                              cap_rights_t rights) {
	struct process* retained;
	struct vmm_info info;
	if (target == NULL || recipient == PROCESS_PID_INVALID) return CAP_ID_INVALID;
	retained = process_acquire(process_pid(target));
	if (retained == NULL || retained != target) {
		process_release(retained);
		return CAP_ID_INVALID;
	}
	if (!vm_space_query_id(process_address_space(retained), mapping_id, &info)) {
		process_release(retained);
		return CAP_ID_INVALID;
	}
	cap_id_t cap = mapping_create(recipient, process_pid(retained), mapping_id, rights);
	process_release(retained);
	return cap;
}

static bool mapping_unmap_state(const struct mapping_state* state) {
	struct process* owner;
	bool            unmapped;
	if (state == NULL) return false;
	owner = process_acquire(state->space_owner);
	if (owner == NULL) return false;
	unmapped = vm_space_unmap(process_address_space(owner), state->mapping_id);
	process_release(owner);
	return unmapped;
}

bool kernel_mapping_discard_unpublished(cap_id_t mapping_cap, process_id_t owner) {
	struct cap_object* object;
	bool               unmapped;
	bool               destroyed;
	if (mapping_cap == CAP_ID_INVALID || owner == PROCESS_PID_INVALID) return false;
	if (cap_object_acquire_for_use(owner, mapping_cap, 0u, &object, NULL) != CAP_OK) return false;
	if (object == NULL || object->handler != mapping_handler) {
		cap_object_release(object);
		return false;
	}
	unmapped  = mapping_unmap_state((const struct mapping_state*)(uintptr_t)object->object_id);
	destroyed = unmapped && cap_object_destroy(object);
	cap_object_release(object);
	return destroyed;
}

static syscall_result_t memory_info_handler(const struct cap_request* req, struct memory_object* memory) {
	const struct memory_info response = {.page_count = memory_object_page_count(memory)};
	return cap_kernel_write_response(req, &response, sizeof(response));
}

static void sync_written_page(struct memory_object* memory, size_t offset) {
	uintptr_t phys;
	if (memory_object_page_phys(memory, offset / PMM_PAGE_SIZE, &phys))
		hal_paging_sync_executable_range((void*)(phys + boot_info.direct_map_offset), PMM_PAGE_SIZE);
}

static syscall_result_t memory_transfer_handler(const struct cap_request* req, struct memory_object* memory,
                                                bool reading) {
	union {
		struct memory_read_request  read;
		struct memory_write_request write;
	} request;
	size_t                       offset;
	size_t                       size;
	uintptr_t                    user_address;
	struct process*              caller;
	struct address_space*        caller_space;
	enum address_transfer_result transfer_result = ADDRESS_TRANSFER_OK;
	bool                         memory_failure  = false;

	if (!cap_kernel_response_fits(req, sizeof(struct memory_transfer_response)))
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	syscall_result_t result =
		reading ? copy_request(req->request, req->request_size, &request.read, sizeof(request.read))
				: copy_request(req->request, req->request_size, &request.write, sizeof(request.write));
	if (result.status != SYSCALL_STATUS_OK) return result;
	offset            = reading ? request.read.offset : request.write.offset;
	size              = reading ? request.read.size : request.write.size;
	user_address      = reading ? request.read.destination : request.write.source;
	size_t page_count = memory_object_page_count(memory);
	if (page_count > SIZE_MAX / PMM_PAGE_SIZE) return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	size_t object_size = page_count * PMM_PAGE_SIZE;
	if (offset > object_size || size > object_size - offset)
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	uint64_t user_end;
	if (add_overflow_u64(user_address, size, &user_end)) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	caller = process_acquire(req->caller);
	if (caller == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	caller_space = process_address_space(caller);
	for (size_t done = 0u; done < size && transfer_result == ADDRESS_TRANSFER_OK;) {
		uint8_t buffer[256];
		size_t  chunk = size - done;
		if (chunk > sizeof(buffer)) chunk = sizeof(buffer);
		size_t page_remaining = PMM_PAGE_SIZE - ((offset + done) & (PMM_PAGE_SIZE - 1u));
		if (chunk > page_remaining) chunk = page_remaining;
		if (reading) {
			if (!memory_object_read(memory, offset + done, buffer, chunk)) memory_failure = true;
			else transfer_result = address_space_copy_to(caller_space, user_address + done, buffer, chunk);
		}
		else {
			transfer_result = address_space_copy_from(caller_space, user_address + done, buffer, chunk);
			if (transfer_result == ADDRESS_TRANSFER_OK) {
				if (!memory_object_write(memory, offset + done, buffer, chunk)) memory_failure = true;
				else sync_written_page(memory, offset + done);
			}
		}
		if (memory_failure) break;
		done += chunk;
	}
	process_release(caller);
	if (memory_failure) return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	if (transfer_result != ADDRESS_TRANSFER_OK) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	const struct memory_transfer_response response = {.bytes_transferred = size};
	return cap_kernel_write_response(req, &response, sizeof(response));
}

static syscall_result_t memory_handler(const struct cap_request* req) {
	struct memory_request_header header;
	struct memory_object*        memory;
	if (req == NULL || req->object_id == 0u) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	syscall_result_t result = copy_request(req->request, req->request_size, &header, sizeof(header));
	if (result.status != SYSCALL_STATUS_OK) return result;
	memory = (struct memory_object*)(uintptr_t)req->object_id;
	switch (header.op) {
	case MEMORY_OP_INFO:
		return memory_info_handler(req, memory);
	case MEMORY_OP_READ:
		if ((req->rights & CAP_READ) == 0u) return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
		return memory_transfer_handler(req, memory, true);
	case MEMORY_OP_WRITE:
		if ((req->rights & CAP_WRITE) == 0u) return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
		return memory_transfer_handler(req, memory, false);
	default:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
}

static syscall_result_t mapping_info_handler(const struct cap_request* req, const struct mapping_state* state) {
	struct process* owner = process_acquire(state->space_owner);
	if (owner == NULL) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	struct mapping_info_response response;
	if (!vm_space_query_id(process_address_space(owner), state->mapping_id, &response.info)) {
		process_release(owner);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	response.info.id        = VMM_ID_INVALID;
	syscall_result_t result = cap_kernel_write_response(req, &response, sizeof(response));
	process_release(owner);
	return result;
}

static syscall_result_t mapping_protect_handler(const struct cap_request* req, const struct mapping_state* state) {
	struct mapping_protect_request request;
	syscall_result_t               result = copy_request(req->request, req->request_size, &request, sizeof(request));
	if (result.status != SYSCALL_STATUS_OK) return result;
	if (!user_prot_is_valid(request.prot)) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	cap_rights_t required = CAP_MAP | protection_rights(request.prot);
	if ((req->rights & required) != required) return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	struct process* owner = process_acquire(state->space_owner);
	if (owner == NULL) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	bool protected = vm_space_protect(process_address_space(owner), state->mapping_id, request.prot);
	process_release(owner);
	return protected ? syscall_result_ok(0u) : syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
}

static syscall_result_t mapping_unmap_handler(const struct cap_request* req, const struct mapping_state* state) {
	struct capability* cap = cap_acquire(req->cap_id);
	if (cap == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	cap_object_id_t object_id = cap->cap_object_id;
	if (!mapping_unmap_state(state)) {
		cap_release(cap);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	bool destroyed = cap_object_destroy_with_id(object_id);
	cap_release(cap);
	return destroyed ? syscall_result_ok(0u) : syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
}

static syscall_result_t mapping_handler(const struct cap_request* req) {
	struct mapping_request_header header;
	if (req == NULL || req->object_id == 0u) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	syscall_result_t result = copy_request(req->request, req->request_size, &header, sizeof(header));
	if (result.status != SYSCALL_STATUS_OK) return result;
	const struct mapping_state* state = (const struct mapping_state*)(uintptr_t)req->object_id;
	switch (header.op) {
	case MAPPING_OP_INFO:
		return mapping_info_handler(req, state);
	case MAPPING_OP_PROTECT:
		return mapping_protect_handler(req, state);
	case MAPPING_OP_UNMAP:
		if ((req->rights & CAP_DESTROY) == 0u) return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
		return mapping_unmap_handler(req, state);
	default:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
}

size_t kernel_mapping_state_size(void) {
	return sizeof(struct mapping_state);
}
