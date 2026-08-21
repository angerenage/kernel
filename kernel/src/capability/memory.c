#include "memory.h"

#include <base/cap.h>
#include <base/memory.h>
#include <base/vmm.h>
#include <core/address_transfer.h>
#include <core/capability.h>
#include <core/memory_object.h>
#include <core/mm.h>
#include <core/pmm.h>
#include <core/process.h>
#include <core/spinlock.h>
#include <core/syscall.h>
#include <core/vm_space.h>
#include <hal/paging.h>
#include <kernel/capability.h>
#include <stdlib.h>
#include <string.h>

struct allocation_state {
	struct spinlock       lock;
	struct memory_object* memory;
	size_t                page_count;
	size_t                mapping_count;
	size_t                operation_count;
	process_id_t          owner;
	vmm_prot_t            prot;
};

struct mapping_state {
	struct spinlock lock;
	void*           backing_context;
	void (*backing_release)(void* context);
	process_id_t space_owner;
	process_id_t cap_owner;
	vmm_id_t     region_id;
	bool         active;
};

static syscall_result_t allocation_handler(const struct cap_request* req);
static syscall_result_t mapping_handler(const struct cap_request* req);
static bool             allocation_process_cleanup(uint64_t object_id, process_id_t process);
static void             allocation_destroy(uint64_t object_id);
static bool             mapping_process_cleanup(uint64_t object_id, process_id_t process);
static void             mapping_destroy(uint64_t object_id);
static syscall_result_t mapping_unmap_handler(struct mapping_state* state);
static cap_id_t         kernel_mapping_create(void*              backing_context, void (*backing_release)(void* context),
                                              struct capability* parent_cap, process_id_t cap_target, process_id_t space_owner,
                                              vmm_id_t region_id, cap_rights_t rights);

static syscall_result_t copy_request(const void* request, size_t request_size, void* out, size_t expected_size) {
	if (request == NULL || out == NULL || request_size < expected_size) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	memcpy(out, request, expected_size);
	return syscall_result_ok(0u);
}

static bool vmm_prot_is_valid(vmm_prot_t prot) {
	return (prot & ~VMM_PROT_VALID_MASK) == 0u;
}

static bool allocation_begin_operation(struct allocation_state* state) {
	bool acquired = false;

	spinlock_lock(&state->lock);
	if (state->memory != NULL) {
		state->operation_count++;
		acquired = true;
	}
	spinlock_unlock(&state->lock);
	return acquired;
}

static void allocation_end_operation(struct allocation_state* state) {
	spinlock_lock(&state->lock);
	if (state->operation_count > 0u) state->operation_count--;
	spinlock_unlock(&state->lock);
}

static bool allocation_mapping_acquire(struct allocation_state* state) {
	bool acquired = false;

	spinlock_lock(&state->lock);
	if (state->memory != NULL) {
		state->mapping_count++;
		acquired = true;
	}
	spinlock_unlock(&state->lock);
	return acquired;
}

static void kernel_allocation_mapping_release(void* context) {
	struct allocation_state* state = context;

	if (state == NULL) return;
	spinlock_lock(&state->lock);
	if (state->mapping_count > 0u) state->mapping_count--;
	spinlock_unlock(&state->lock);
}

static syscall_result_t allocation_map_into(struct allocation_state* state, struct capability* allocation_cap,
                                            process_id_t cap_target, struct process* target, uintptr_t address,
                                            cap_id_t* out_mapping_cap) {
	struct address_space* space;
	struct process*       retained_target;
	vmm_id_t              new_id = VMM_ID_INVALID;
	cap_id_t              mapping_cap;
	process_id_t          space_owner;

	if (state == NULL || allocation_cap == NULL || cap_target == PROCESS_PID_INVALID || target == NULL ||
	    out_mapping_cap == NULL) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	if (address != 0u && (address & (PMM_PAGE_SIZE - 1u)) != 0u) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	space_owner     = process_pid(target);
	retained_target = process_acquire(space_owner);
	if (retained_target == NULL || retained_target != target) {
		process_release(retained_target);
		return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	}
	space = process_address_space(retained_target);
	if (!allocation_mapping_acquire(state)) {
		process_release(retained_target);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	if (!vm_space_map(space,
	                  &(const struct vm_map_request){.memory         = state->memory,
	                                                 .page_count     = state->page_count,
	                                                 .requested_base = address,
	                                                 .align_pages    = 1u,
	                                                 .prot           = state->prot},
	                  &new_id,
	                  NULL)) {
		kernel_allocation_mapping_release(state);
		process_release(retained_target);
		return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	}

	mapping_cap = kernel_mapping_create(state,
	                                    kernel_allocation_mapping_release,
	                                    allocation_cap,
	                                    cap_target,
	                                    space_owner,
	                                    new_id,
	                                    CAP_READ | CAP_MAP | CAP_DESTROY);
	if (mapping_cap == CAP_ID_INVALID) {
		(void)vm_space_unmap(space, new_id);
		kernel_allocation_mapping_release(state);
		process_release(retained_target);
		return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	}

	*out_mapping_cap = mapping_cap;
	process_release(retained_target);
	return syscall_result_ok(0u);
}

syscall_result_t kernel_map_allocation(cap_id_t allocation_cap_id, process_id_t caller, struct process* target,
                                       uintptr_t address, cap_id_t* out_mapping_cap) {
	struct capability* cap;
	struct cap_object* object;
	enum cap_result    cap_result;
	syscall_result_t   result;

	if (allocation_cap_id == CAP_ID_INVALID || caller == PROCESS_PID_INVALID || target == NULL ||
	    out_mapping_cap == NULL) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	cap = cap_acquire(allocation_cap_id);
	if (cap == NULL) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	cap_result = cap_is_authorized(caller, cap);
	if (cap_result != CAP_OK) {
		cap_release(cap);
		return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	}
	cap_result = cap_is_valid(cap);
	if (cap_result != CAP_OK) {
		cap_release(cap);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	if ((cap_rights(cap) & CAP_MAP) == 0u) {
		cap_release(cap);
		return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	}

	object = cap_object_acquire(cap->cap_object_id);
	if (object == NULL || object->handler != allocation_handler) {
		cap_object_release(object);
		cap_release(cap);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	result = allocation_map_into(
		(struct allocation_state*)(uintptr_t)object->object_id, cap, caller, target, address, out_mapping_cap);
	cap_object_release(object);
	cap_release(cap);
	return result;
}

cap_id_t kernel_allocate_memory(cap_rights_t rights, size_t page_count, vmm_prot_t prot) {
	cap_object_id_t          object_id;
	cap_id_t                 cap_id;
	struct allocation_state* state;
	struct process*          caller;
	struct memory_object*    memory;

	if (page_count == 0u || page_count > SIZE_MAX / PMM_PAGE_SIZE) return CAP_ID_INVALID;
	if (!vmm_prot_is_valid(prot) || (prot & VMM_PROT_GLOBAL) != 0u) return CAP_ID_INVALID;
	if (!memory_object_create_owned(page_count, &memory)) return CAP_ID_INVALID;

	state = malloc(sizeof(*state));
	if (state == NULL) {
		memory_object_release(memory);
		return CAP_ID_INVALID;
	}

	spinlock_init(&state->lock);
	state->memory          = memory;
	state->page_count      = page_count;
	state->mapping_count   = 0u;
	state->operation_count = 0u;
	state->owner           = PROCESS_PID_INVALID;
	state->prot            = prot;

	object_id = cap_object_create_kernel_managed(
		(uint64_t)(uintptr_t)state, allocation_handler, allocation_process_cleanup, allocation_destroy, NULL);
	if (object_id == CAP_OBJECT_ID_INVALID) {
		free(state);
		memory_object_release(memory);
		return CAP_ID_INVALID;
	}

	caller = process_current();
	if (caller == NULL) {
		(void)cap_object_destroy_with_id(object_id);
		return CAP_ID_INVALID;
	}
	state->owner = process_pid(caller);

	cap_id = cap_create(object_id, process_pid(caller), rights, NULL);
	if (cap_id == CAP_ID_INVALID) {
		(void)cap_object_destroy_with_id(object_id);
		return CAP_ID_INVALID;
	}
	return cap_id;
}

cap_id_t kernel_mapping_grant(struct process* target, process_id_t recipient, vmm_id_t region_id, cap_rights_t rights) {
	process_id_t          owner;
	struct address_space* space;
	struct vmm_info       info;

	if (target == NULL || recipient == PROCESS_PID_INVALID) return CAP_ID_INVALID;
	owner                    = process_pid(target);
	struct process* retained = process_acquire(owner);
	if (retained == NULL || retained != target) {
		process_release(retained);
		return CAP_ID_INVALID;
	}
	space = process_address_space(retained);
	if (!vm_space_query_id(space, region_id, &info)) {
		process_release(retained);
		return CAP_ID_INVALID;
	}
	cap_id_t cap = kernel_mapping_create(NULL, NULL, NULL, recipient, owner, region_id, rights);
	process_release(retained);
	return cap;
}

bool kernel_mapping_discard_unpublished(cap_id_t mapping_cap, process_id_t owner) {
	struct capability*    cap;
	struct cap_object*    object;
	struct mapping_state* state;
	cap_object_id_t       object_id;
	syscall_result_t      result;

	if (mapping_cap == CAP_ID_INVALID || owner == PROCESS_PID_INVALID) return false;
	cap = cap_acquire(mapping_cap);
	if (cap == NULL) return false;
	if (cap_is_authorized(owner, cap) != CAP_OK) {
		cap_release(cap);
		return false;
	}
	object_id = cap->cap_object_id;
	object    = cap_object_acquire(object_id);
	if (object == NULL || object->handler != mapping_handler) {
		cap_object_release(object);
		cap_release(cap);
		return false;
	}
	cap_release(cap);
	state  = (struct mapping_state*)(uintptr_t)object->object_id;
	result = mapping_unmap_handler(state);
	cap_object_release(object);
	if (result.status != SYSCALL_STATUS_OK) return false;
	if (!cap_destroy_by_id(mapping_cap)) return false;
	if (!cap_object_destroy_with_id(object_id)) return false;
	return true;
}

static syscall_result_t allocation_free_handler(const struct cap_request* req, struct allocation_state* state) {
	struct memory_object* memory;
	cap_object_id_t       object_id;

	spinlock_lock(&state->lock);
	if (state->memory == NULL || state->mapping_count != 0u || state->operation_count != 0u) {
		spinlock_unlock(&state->lock);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	memory        = state->memory;
	state->memory = NULL;
	spinlock_unlock(&state->lock);
	memory_object_release(memory);
	object_id              = CAP_OBJECT_ID_INVALID;
	struct capability* cap = cap_acquire(req->cap_id);
	if (cap != NULL) {
		object_id = cap->cap_object_id;
		cap_release(cap);
	}
	(void)cap_destroy_by_id(req->cap_id);
	if (object_id != CAP_OBJECT_ID_INVALID) (void)cap_object_destroy_with_id(object_id);
	return syscall_result_ok(0u);
}

static syscall_result_t allocation_read_handler(const struct cap_request* req, struct allocation_state* state) {
	struct allocation_read_response response;

	if (!allocation_begin_operation(state)) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	response.info = (struct vmm_info){
		.id          = VMM_ID_INVALID,
		.base        = NULL,
		.page_count  = state->page_count,
		.prot        = state->prot,
		.guard_pages = 0u,
	};

	allocation_end_operation(state);
	return cap_kernel_write_response(req, &response, sizeof(response));
}

static syscall_result_t allocation_copy_handler(const struct cap_request* req, struct allocation_state* state,
                                                bool from_allocation) {
	union {
		struct allocation_copy_from_request from;
		struct allocation_copy_to_request   to;
	} request;
	struct address_space*        caller_space;
	struct process*              caller;
	uintptr_t                    allocation_size;
	uintptr_t                    offset;
	size_t                       size;
	enum address_transfer_result transfer_result;
	syscall_result_t             copy_result;

	if (!cap_kernel_response_fits(req, sizeof(struct allocation_copy_response))) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	if (from_allocation) {
		copy_result = copy_request(req->request, req->request_size, &request.from, sizeof(request.from));
		if (copy_result.status != SYSCALL_STATUS_OK) return copy_result;
		offset = request.from.src_offset;
		size   = request.from.size;
	}
	else {
		copy_result = copy_request(req->request, req->request_size, &request.to, sizeof(request.to));
		if (copy_result.status != SYSCALL_STATUS_OK) return copy_result;
		offset = request.to.dst_offset;
		size   = request.to.size;
	}

	caller = process_acquire(req->caller);
	if (caller == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	caller_space = process_address_space(caller);
	if (size == 0u) {
		const struct allocation_copy_response response = {.bytes_copied = 0u};
		copy_result                                    = cap_kernel_write_response(req, &response, sizeof(response));
		process_release(caller);
		return copy_result;
	}
	if (!allocation_begin_operation(state)) {
		process_release(caller);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	allocation_size = state->page_count * (uintptr_t)PMM_PAGE_SIZE;
	if (offset >= allocation_size || size > allocation_size - offset) {
		allocation_end_operation(state);
		process_release(caller);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	transfer_result = ADDRESS_TRANSFER_OK;
	for (size_t done = 0u; done < size && transfer_result == ADDRESS_TRANSFER_OK;) {
		uint8_t buffer[256];
		size_t  chunk = size - done;
		if (chunk > sizeof(buffer)) chunk = sizeof(buffer);
		if (from_allocation) {
			if (!memory_object_read(state->memory, offset + done, buffer, chunk)) {
				transfer_result = ADDRESS_TRANSFER_FAULT_FAILED;
				break;
			}
			transfer_result = address_space_copy_to(caller_space, request.from.dst_address + done, buffer, chunk);
		}
		else {
			transfer_result = address_space_copy_from(caller_space, request.to.src_address + done, buffer, chunk);
			if (transfer_result == ADDRESS_TRANSFER_OK &&
			    !memory_object_write(state->memory, offset + done, buffer, chunk))
				transfer_result = ADDRESS_TRANSFER_FAULT_FAILED;
		}
		done += chunk;
	}
	if (transfer_result == ADDRESS_TRANSFER_OK && !from_allocation && (state->prot & VMM_PROT_EXEC) != 0u) {
		size_t first = offset / PMM_PAGE_SIZE;
		size_t last  = (offset + size - 1u) / PMM_PAGE_SIZE;
		for (size_t page = first; page <= last; page++) {
			uintptr_t phys;
			if (memory_object_page_phys(state->memory, page, &phys))
				hal_paging_sync_executable_range((void*)(phys + boot_info.direct_map_offset), PMM_PAGE_SIZE);
		}
	}
	allocation_end_operation(state);
	process_release(caller);
	if (transfer_result == ADDRESS_TRANSFER_FAULT_FAILED) {
		return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	}
	if (transfer_result != ADDRESS_TRANSFER_OK) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	struct allocation_copy_response response = {.bytes_copied = size};
	return cap_kernel_write_response(req, &response, sizeof(response));
}

static syscall_result_t allocation_handler(const struct cap_request* req) {
	struct allocation_state*         state;
	struct allocation_request_header header;
	cap_rights_t                     required_rights;
	syscall_result_t                 copy_result;

	if (req == NULL) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	state = (struct allocation_state*)(uintptr_t)req->object_id;
	if (state == NULL) return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);

	copy_result = copy_request(req->request, req->request_size, &header, sizeof(header));
	if (copy_result.status != SYSCALL_STATUS_OK) return copy_result;

	switch (header.op) {
	case ALLOCATION_OP_FREE:
		required_rights = CAP_DESTROY;
		break;
	case ALLOCATION_OP_READ:
	case ALLOCATION_OP_COPY_FROM:
		required_rights = CAP_READ;
		break;
	case ALLOCATION_OP_COPY_TO:
		required_rights = CAP_WRITE;
		break;
	default:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	if ((req->rights & required_rights) != required_rights) {
		return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	}

	switch (header.op) {
	case ALLOCATION_OP_FREE:
		return allocation_free_handler(req, state);
	case ALLOCATION_OP_READ:
		return allocation_read_handler(req, state);
	case ALLOCATION_OP_COPY_FROM:
		return allocation_copy_handler(req, state, true);
	case ALLOCATION_OP_COPY_TO:
		return allocation_copy_handler(req, state, false);
	default:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
}

static syscall_result_t mapping_read_handler(const struct cap_request* req, struct mapping_state* state) {
	struct mapping_read_response response;
	struct process*              owner = process_acquire(state->space_owner);

	if (owner == NULL) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);

	spinlock_lock(&state->lock);
	if (!state->active || !vm_space_query_id(process_address_space(owner), state->region_id, &response.info)) {
		spinlock_unlock(&state->lock);
		process_release(owner);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	response.info.id = VMM_ID_INVALID;
	spinlock_unlock(&state->lock);

	syscall_result_t result = cap_kernel_write_response(req, &response, sizeof(response));
	process_release(owner);
	return result;
}

static syscall_result_t mapping_protect_handler(const struct cap_request* req, struct mapping_state* state) {
	struct mapping_protect_request request;
	syscall_result_t               copy_result;
	struct process*                owner;

	copy_result = copy_request(req->request, req->request_size, &request, sizeof(request));
	if (copy_result.status != SYSCALL_STATUS_OK) return copy_result;
	if (!vmm_prot_is_valid(request.prot)) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	owner = process_acquire(state->space_owner);
	if (owner == NULL) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);

	spinlock_lock(&state->lock);
	if (!state->active || !vm_space_protect(process_address_space(owner), state->region_id, request.prot)) {
		spinlock_unlock(&state->lock);
		process_release(owner);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	spinlock_unlock(&state->lock);
	process_release(owner);
	return syscall_result_ok(0u);
}

static syscall_result_t mapping_unmap_handler(struct mapping_state* state) {
	struct process* owner = process_acquire(state->space_owner);

	spinlock_lock(&state->lock);
	if (!state->active) {
		spinlock_unlock(&state->lock);
		process_release(owner);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	if (owner != NULL && !vm_space_unmap(process_address_space(owner), state->region_id)) {
		spinlock_unlock(&state->lock);
		process_release(owner);
		return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	}
	state->active = false;
	spinlock_unlock(&state->lock);
	process_release(owner);
	if (state->backing_release != NULL) state->backing_release(state->backing_context);
	return syscall_result_ok(0u);
}

static bool allocation_process_cleanup(uint64_t object_id, process_id_t process) {
	struct allocation_state* state = (struct allocation_state*)(uintptr_t)object_id;
	bool                     ready;

	if (state == NULL || state->owner != process) return false;
	spinlock_lock(&state->lock);
	ready = state->mapping_count == 0u && state->operation_count == 0u;
	spinlock_unlock(&state->lock);
	return ready;
}

static void allocation_destroy(uint64_t object_id) {
	struct allocation_state* state = (struct allocation_state*)(uintptr_t)object_id;
	struct memory_object*    memory;

	if (state == NULL) return;
	spinlock_lock(&state->lock);
	memory        = state->memory;
	state->memory = NULL;
	spinlock_unlock(&state->lock);
	memory_object_release(memory);
	free(state);
}

static bool mapping_process_cleanup(uint64_t object_id, process_id_t process) {
	struct mapping_state* state = (struct mapping_state*)(uintptr_t)object_id;

	if (state == NULL || (state->space_owner != process && state->cap_owner != process)) return false;
	(void)mapping_unmap_handler(state);
	return true;
}

static void mapping_destroy(uint64_t object_id) {
	struct mapping_state* state = (struct mapping_state*)(uintptr_t)object_id;
	if (state == NULL) return;
	if (state->active) (void)mapping_unmap_handler(state);
	free(state);
}

static syscall_result_t mapping_handler(const struct cap_request* req) {
	struct mapping_request_header header;
	struct mapping_state*         state;
	cap_rights_t                  required_rights;
	syscall_result_t              copy_result;

	if (req == NULL) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	state = (struct mapping_state*)(uintptr_t)req->object_id;
	if (state == NULL) return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);

	copy_result = copy_request(req->request, req->request_size, &header, sizeof(header));
	if (copy_result.status != SYSCALL_STATUS_OK) return copy_result;

	switch (header.op) {
	case MAPPING_OP_READ:
		required_rights = CAP_READ;
		break;
	case MAPPING_OP_PROTECT:
		required_rights = CAP_MAP;
		break;
	case MAPPING_OP_UNMAP:
		required_rights = CAP_DESTROY;
		break;
	default:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	if ((req->rights & required_rights) != required_rights) {
		return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	}

	switch (header.op) {
	case MAPPING_OP_READ:
		return mapping_read_handler(req, state);
	case MAPPING_OP_PROTECT:
		return mapping_protect_handler(req, state);
	case MAPPING_OP_UNMAP:
		copy_result = mapping_unmap_handler(state);
		if (copy_result.status == SYSCALL_STATUS_OK) {
			cap_object_id_t    object_id = CAP_OBJECT_ID_INVALID;
			struct capability* cap       = cap_acquire(req->cap_id);
			if (cap != NULL) {
				object_id = cap->cap_object_id;
				cap_release(cap);
			}
			(void)cap_destroy_by_id(req->cap_id);
			if (object_id != CAP_OBJECT_ID_INVALID) (void)cap_object_destroy_with_id(object_id);
		}
		return copy_result;
	default:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
}

static cap_id_t kernel_mapping_create(void*              backing_context, void (*backing_release)(void* context),
                                      struct capability* parent_cap, process_id_t cap_target, process_id_t space_owner,
                                      vmm_id_t region_id, cap_rights_t rights) {
	struct mapping_state* state;
	cap_object_id_t       object_id;
	cap_id_t              cap_id;

	if (cap_target == PROCESS_PID_INVALID || space_owner == PROCESS_PID_INVALID || region_id == VMM_ID_INVALID ||
	    (rights & ~(CAP_READ | CAP_MAP | CAP_DESTROY)) != 0u) {
		return CAP_ID_INVALID;
	}

	state = malloc(sizeof(*state));
	if (state == NULL) return CAP_ID_INVALID;
	spinlock_init(&state->lock);
	state->backing_context = backing_context;
	state->backing_release = backing_release;
	state->space_owner     = space_owner;
	state->cap_owner       = cap_target;
	state->region_id       = region_id;
	state->active          = true;

	object_id = cap_object_create_kernel_managed(
		(uint64_t)(uintptr_t)state, mapping_handler, mapping_process_cleanup, mapping_destroy, NULL);
	if (object_id == CAP_OBJECT_ID_INVALID) {
		free(state);
		return CAP_ID_INVALID;
	}

	cap_id = cap_create(object_id, cap_target, CAP_CALL | rights, parent_cap);
	if (cap_id == CAP_ID_INVALID) {
		(void)cap_object_destroy_with_id(object_id);
		return CAP_ID_INVALID;
	}

	return cap_id;
}
