#include "memory.h"

#include <base/cap.h>
#include <base/memory.h>
#include <base/vmm.h>
#include <core/address_transfer.h>
#include <core/capability.h>
#include <core/mm.h>
#include <core/pmm.h>
#include <core/process.h>
#include <core/spinlock.h>
#include <core/syscall.h>
#include <core/vmm.h>
#include <kernel/capability.h>
#include <stdlib.h>
#include <string.h>

struct allocation_state {
	struct spinlock lock;
	uintptr_t       phys_base;
	size_t          page_count;
	size_t          mapping_count;
	size_t          operation_count;
	vmm_prot_t      prot;
	enum vmm_kind   kind;
};

struct mapping_state {
	struct spinlock          lock;
	struct allocation_state* allocation;
	struct address_space*    space;
	process_id_t             space_owner;
	vmm_id_t                 region_id;
	enum vmm_kind            allocation_kind;
	bool                     active;
};

static syscall_result_t allocation_handler(const struct cap_request* req);
static syscall_result_t mapping_handler(const struct cap_request* req);
static cap_id_t         kernel_mapping_create(struct allocation_state* allocation, struct capability* allocation_cap,
                                              process_id_t cap_target, process_id_t space_owner, struct address_space* space,
                                              vmm_id_t region_id, enum vmm_kind allocation_kind);

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

static bool vmm_kind_is_valid(enum vmm_kind kind) {
	switch (kind) {
	case VMM_KIND_GENERIC:
	case VMM_KIND_HEAP:
	case VMM_KIND_STACK:
		return true;
	case VMM_KIND_MMIO:
	case VMM_KIND_KERNEL_TEXT:
	case VMM_KIND_KERNEL_RODATA:
	case VMM_KIND_KERNEL_DATA:
	case VMM_KIND_PHYSICAL:
	default:
		return false;
	}
}

static struct address_space* caller_address_space(process_id_t caller_pid) {
	struct process* caller = process_lookup(caller_pid);
	return caller == NULL ? NULL : process_address_space(caller);
}

static bool allocation_begin_operation(struct allocation_state* state) {
	bool acquired = false;

	spinlock_lock(&state->lock);
	if (state->phys_base != 0u) {
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
	if (state->phys_base != 0u) {
		state->mapping_count++;
		acquired = true;
	}
	spinlock_unlock(&state->lock);
	return acquired;
}

static void kernel_allocation_mapping_release(struct allocation_state* state) {
	if (state == NULL) return;
	spinlock_lock(&state->lock);
	if (state->mapping_count > 0u) state->mapping_count--;
	spinlock_unlock(&state->lock);
}

static syscall_result_t allocation_map_into(struct allocation_state* state, struct capability* allocation_cap,
                                            process_id_t cap_target, struct process* target, uintptr_t address,
                                            cap_id_t* out_mapping_cap) {
	struct address_space* space;
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

	space_owner = process_pid(target);
	space       = process_address_space(target);
	if (space_owner == PROCESS_PID_INVALID || process_lookup(space_owner) != target || space == NULL) {
		return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	}
	if (!allocation_mapping_acquire(state)) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);

	if (!vmm_alloc_phys(space, state->phys_base, state->page_count, state->prot, (void*)address, &new_id, NULL)) {
		kernel_allocation_mapping_release(state);
		return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	}

	mapping_cap = kernel_mapping_create(state, allocation_cap, cap_target, space_owner, space, new_id, state->kind);
	if (mapping_cap == CAP_ID_INVALID) {
		(void)vmm_free(space, new_id);
		kernel_allocation_mapping_release(state);
		return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	}

	*out_mapping_cap = mapping_cap;
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

	cap = cap_lookup(allocation_cap_id);
	if (cap == NULL) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	cap_result = cap_is_authorized(caller, cap);
	if (cap_result != CAP_OK) return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	cap_result = cap_is_valid(cap);
	if (cap_result != CAP_OK) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	if ((cap->rights & CAP_MAP) == 0u) return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);

	object = cap_object_acquire(cap->cap_object_id);
	if (object == NULL || object->handler != allocation_handler) {
		cap_object_release(object);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	result = allocation_map_into(
		(struct allocation_state*)(uintptr_t)object->object_id, cap, caller, target, address, out_mapping_cap);
	cap_object_release(object);
	return result;
}

cap_id_t kernel_allocate_memory(cap_rights_t rights, size_t page_count, vmm_prot_t prot, enum vmm_kind kind) {
	struct cap_object*       object;
	struct capability*       cap;
	struct allocation_state* state;
	struct process*          caller;
	uintptr_t                phys_base;

	if (page_count == 0u || page_count > SIZE_MAX / PMM_PAGE_SIZE) return CAP_ID_INVALID;
	if (!vmm_prot_is_valid(prot) || !vmm_kind_is_valid(kind)) return CAP_ID_INVALID;
	if (!pmm_alloc_pages(page_count, &phys_base)) return CAP_ID_INVALID;

	state = malloc(sizeof(*state));
	if (state == NULL) {
		(void)pmm_free_pages(phys_base, page_count);
		return CAP_ID_INVALID;
	}

	spinlock_init(&state->lock);
	state->phys_base       = phys_base;
	state->page_count      = page_count;
	state->mapping_count   = 0u;
	state->operation_count = 0u;
	state->prot            = prot;
	state->kind            = kind;

	object = cap_object_create_kernel((uint64_t)(uintptr_t)state, allocation_handler);
	if (object == NULL) {
		free(state);
		(void)pmm_free_pages(phys_base, page_count);
		return CAP_ID_INVALID;
	}

	caller = process_current();
	if (caller == NULL) {
		(void)cap_object_destroy(object);
		free(state);
		(void)pmm_free_pages(phys_base, page_count);
		return CAP_ID_INVALID;
	}

	cap = cap_create(object->cap_object_id, process_pid(caller), rights, NULL);
	if (cap == NULL) {
		(void)cap_object_destroy(object);
		free(state);
		(void)pmm_free_pages(phys_base, page_count);
		return CAP_ID_INVALID;
	}
	return cap->cap_id;
}

static syscall_result_t allocation_free_handler(struct allocation_state* state) {
	uintptr_t phys_base;

	spinlock_lock(&state->lock);
	if (state->phys_base == 0u || state->mapping_count != 0u || state->operation_count != 0u) {
		spinlock_unlock(&state->lock);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	phys_base        = state->phys_base;
	state->phys_base = 0u;
	spinlock_unlock(&state->lock);

	if (!pmm_free_pages(phys_base, state->page_count)) {
		spinlock_lock(&state->lock);
		state->phys_base = phys_base;
		spinlock_unlock(&state->lock);
		return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	}
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
		.kind        = state->kind,
		.guard_pages = 0u,
		.state       = VMM_STATE_RESERVED,
		.first_phys  = state->phys_base,
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
	uintptr_t                    allocation_size;
	uintptr_t                    allocation_address;
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

	caller_space = caller_address_space(req->caller);
	if (caller_space == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	if (size == 0u) {
		const struct allocation_copy_response response = {.bytes_copied = 0u};
		return cap_kernel_write_response(req, &response, sizeof(response));
	}
	if (!allocation_begin_operation(state)) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);

	allocation_size = state->page_count * (uintptr_t)PMM_PAGE_SIZE;
	if (offset >= allocation_size || size > allocation_size - offset) {
		allocation_end_operation(state);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	allocation_address = state->phys_base + boot_info.direct_map_offset + offset;
	if (from_allocation) {
		transfer_result =
			address_space_copy_to(caller_space, request.from.dst_address, (const void*)allocation_address, size);
	}
	else {
		transfer_result =
			address_space_copy_from(caller_space, request.to.src_address, (void*)allocation_address, size);
	}
	allocation_end_operation(state);
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
		return allocation_free_handler(state);
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

static bool mapping_owner_is_alive(const struct mapping_state* state) {
	struct process* owner = process_lookup(state->space_owner);
	return owner != NULL && process_address_space(owner) == state->space;
}

static syscall_result_t mapping_read_handler(const struct cap_request* req, struct mapping_state* state) {
	struct mapping_read_response response;

	if (!mapping_owner_is_alive(state)) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	spinlock_lock(&state->lock);
	if (!state->active || !vmm_query_id(state->space, state->region_id, &response.info)) {
		spinlock_unlock(&state->lock);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	response.info.id   = VMM_ID_INVALID;
	response.info.kind = state->allocation_kind;
	spinlock_unlock(&state->lock);

	return cap_kernel_write_response(req, &response, sizeof(response));
}

static syscall_result_t mapping_protect_handler(const struct cap_request* req, struct mapping_state* state) {
	struct mapping_protect_request request;
	syscall_result_t               copy_result;

	copy_result = copy_request(req->request, req->request_size, &request, sizeof(request));
	if (copy_result.status != SYSCALL_STATUS_OK) return copy_result;
	if (!vmm_prot_is_valid(request.prot)) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	if (!mapping_owner_is_alive(state)) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	spinlock_lock(&state->lock);
	if (!state->active || !vmm_protect(state->space, state->region_id, request.prot)) {
		spinlock_unlock(&state->lock);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	spinlock_unlock(&state->lock);
	return syscall_result_ok(0u);
}

static syscall_result_t mapping_unmap_handler(struct mapping_state* state) {
	bool owner_alive = mapping_owner_is_alive(state);

	spinlock_lock(&state->lock);
	if (!state->active) {
		spinlock_unlock(&state->lock);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	if (owner_alive && !vmm_free(state->space, state->region_id)) {
		spinlock_unlock(&state->lock);
		return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	}
	state->active = false;
	spinlock_unlock(&state->lock);
	kernel_allocation_mapping_release(state->allocation);
	return syscall_result_ok(0u);
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
		return mapping_unmap_handler(state);
	default:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
}

static cap_id_t kernel_mapping_create(struct allocation_state* allocation, struct capability* allocation_cap,
                                      process_id_t cap_target, process_id_t space_owner, struct address_space* space,
                                      vmm_id_t region_id, enum vmm_kind allocation_kind) {
	struct mapping_state* state;
	struct cap_object*    object;
	struct capability*    cap;

	if (allocation == NULL || allocation_cap == NULL || cap_target == PROCESS_PID_INVALID ||
	    space_owner == PROCESS_PID_INVALID || space == NULL || region_id == VMM_ID_INVALID) {
		return CAP_ID_INVALID;
	}

	state = malloc(sizeof(*state));
	if (state == NULL) return CAP_ID_INVALID;
	spinlock_init(&state->lock);
	state->allocation      = allocation;
	state->space           = space;
	state->space_owner     = space_owner;
	state->region_id       = region_id;
	state->allocation_kind = allocation_kind;
	state->active          = true;

	object = cap_object_create_kernel((uint64_t)(uintptr_t)state, mapping_handler);
	if (object == NULL) {
		free(state);
		return CAP_ID_INVALID;
	}

	cap = cap_create(object->cap_object_id, cap_target, CAP_CALL | CAP_READ | CAP_MAP | CAP_DESTROY, allocation_cap);
	if (cap == NULL) {
		(void)cap_object_destroy(object);
		free(state);
		return CAP_ID_INVALID;
	}

	return cap->cap_id;
}
