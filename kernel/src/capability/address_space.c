#include "address_space.h"

#include <base/address_space.h>
#include <base/cap.h>
#include <base/vmm.h>
#include <core/address_transfer.h>
#include <core/capability.h>
#include <core/message.h>
#include <core/pmm.h>
#include <core/process.h>
#include <core/syscall.h>
#include <core/vmm.h>
#include <stdlib.h>
#include <string.h>

/* Copy request data from caller's address space into a kernel buffer. */
static syscall_result_t copy_request_from_caller(process_id_t caller_pid, const void* req_ptr, size_t req_size,
                                                 void* out_buf) {
	struct process*              caller;
	struct address_space*        space;
	enum address_transfer_result result;

	if (req_ptr == NULL || req_size == 0u || out_buf == NULL) return syscall_result_ok(0u);

	caller = process_lookup(caller_pid);
	if (caller == NULL) return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);

	space = process_address_space(caller);
	if (space == NULL) {
		memcpy(out_buf, req_ptr, req_size);
		return syscall_result_ok(0u);
	}

	result = address_space_copy_from(space, (uintptr_t)req_ptr, out_buf, req_size);
	if (result != ADDRESS_TRANSFER_OK) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);

	return syscall_result_ok(0u);
}

/* Send a response payload back to the caller process. */
static syscall_result_t send_response_to_caller(process_id_t caller_pid, const void* response, size_t response_size) {
	struct process* caller = process_lookup(caller_pid);
	if (caller == NULL) return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);

	if (message_queue_send(&caller->message_queue, caller_pid, response, response_size) != MESSAGE_OK) {
		return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	}

	return syscall_result_ok(0u);
}

static bool vmm_prot_is_valid(vmm_prot_t prot) {
	return (prot & ~VMM_PROT_VALID_MASK) == 0;
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

static syscall_result_t address_space_reserve_handler(const struct cap_request* req, struct address_space* space) {
	struct address_space_reserve_request request;
	struct vmm_alloc_params              params;
	vmm_id_t                             id   = VMM_ID_INVALID;
	void*                                base = NULL;
	syscall_result_t                     copy_result;

	copy_result = copy_request_from_caller(req->caller, req->request, req->request_size, &request);
	if (copy_result.status != SYSCALL_STATUS_OK) return copy_result;

	if (request.page_count == 0u) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	if (!vmm_prot_is_valid(request.prot)) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	if (!vmm_kind_is_valid(request.kind)) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	if ((request.map_flags & ~((uint64_t)VMM_MAP_LAZY)) != 0u) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	params = (struct vmm_alloc_params){
		.page_count  = request.page_count,
		.align_pages = VMM_MIN_ALIGN_PAGES,
		.prot        = request.prot,
		.kind        = request.kind,
		.guard_pages = 0u,
		.map_flags   = request.map_flags,
	};

	if (!vmm_alloc(space, &params, &id, &base)) return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);

	struct address_space_reserve_response response = {id, base};
	return send_response_to_caller(req->caller, &response, sizeof(response));
}

static syscall_result_t address_space_reserve_at_handler(const struct cap_request* req, struct address_space* space) {
	struct address_space_reserve_at_request request;
	struct vmm_alloc_params                 params;
	vmm_id_t                                id = VMM_ID_INVALID;
	syscall_result_t                        copy_result;

	copy_result = copy_request_from_caller(req->caller, req->request, req->request_size, &request);
	if (copy_result.status != SYSCALL_STATUS_OK) return copy_result;

	if (request.address == 0u || (request.address & (PMM_PAGE_SIZE - 1u)) != 0u) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	if (request.page_count == 0u) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	if (!vmm_prot_is_valid(request.prot)) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	if (!vmm_kind_is_valid(request.kind)) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	if ((request.map_flags & ~((uint64_t)VMM_MAP_LAZY)) != 0u) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	params = (struct vmm_alloc_params){
		.page_count  = request.page_count,
		.align_pages = VMM_MIN_ALIGN_PAGES,
		.prot        = request.prot,
		.kind        = request.kind,
		.guard_pages = 0u,
		.map_flags   = request.map_flags,
	};

	if (!vmm_alloc_at(space, (void*)request.address, &params, &id)) {
		return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	}

	struct address_space_reserve_response response = {id, (void*)request.address};
	return send_response_to_caller(req->caller, &response, sizeof(response));
}

static syscall_result_t address_space_free_handler(const struct cap_request* req, struct address_space* space) {
	struct address_space_free_request request;
	syscall_result_t                  copy_result;

	copy_result = copy_request_from_caller(req->caller, req->request, req->request_size, &request);
	if (copy_result.status != SYSCALL_STATUS_OK) return copy_result;

	if (request.id == VMM_ID_INVALID) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);

	if (!vmm_free(space, request.id)) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);

	return syscall_result_ok(0u);
}

static syscall_result_t address_space_map_handler(const struct cap_request* req, struct address_space* space) {
	struct address_space_map_request request;
	syscall_result_t                 copy_result;

	copy_result = copy_request_from_caller(req->caller, req->request, req->request_size, &request);
	if (copy_result.status != SYSCALL_STATUS_OK) return copy_result;

	if (request.id == VMM_ID_INVALID) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);

	if (!vmm_map(space, request.id)) return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);

	return syscall_result_ok(0u);
}

static syscall_result_t address_space_unmap_handler(const struct cap_request* req, struct address_space* space) {
	struct address_space_unmap_request request;
	syscall_result_t                   copy_result;

	copy_result = copy_request_from_caller(req->caller, req->request, req->request_size, &request);
	if (copy_result.status != SYSCALL_STATUS_OK) return copy_result;

	if (request.id == VMM_ID_INVALID) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);

	if (!vmm_unmap(space, request.id, request.free_pages)) {
		return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	}

	return syscall_result_ok(0u);
}

static syscall_result_t address_space_protect_handler(const struct cap_request* req, struct address_space* space) {
	struct address_space_protect_request request;
	syscall_result_t                     copy_result;

	copy_result = copy_request_from_caller(req->caller, req->request, req->request_size, &request);
	if (copy_result.status != SYSCALL_STATUS_OK) return copy_result;

	if (request.id == VMM_ID_INVALID) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	if (!vmm_prot_is_valid(request.prot)) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);

	if (!vmm_protect(space, request.id, request.prot)) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	return syscall_result_ok(0u);
}

static syscall_result_t address_space_query_handler(const struct cap_request* req, struct address_space* space) {
	struct address_space_query_request request;
	struct vmm_info                    info;
	syscall_result_t                   copy_result;

	copy_result = copy_request_from_caller(req->caller, req->request, req->request_size, &request);
	if (copy_result.status != SYSCALL_STATUS_OK) return copy_result;

	if (request.id == VMM_ID_INVALID) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);

	if (!vmm_query_id(space, request.id, &info)) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);

	return send_response_to_caller(req->caller, &info, sizeof(info));
}

static syscall_result_t address_space_copy_from_handler(const struct cap_request* req, struct address_space* space) {
	struct address_space_copy_from_request request;
	size_t                                 size;
	syscall_result_t                       copy_result;
	struct address_space*                  caller_space;

	copy_result = copy_request_from_caller(req->caller, req->request, req->request_size, &request);
	if (copy_result.status != SYSCALL_STATUS_OK) return copy_result;

	size = request.size;
	if (size == 0u) return syscall_result_ok(0u);

	caller_space = process_address_space(process_lookup(req->caller));
	if (caller_space == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);

	enum address_transfer_result transfer_result;
	transfer_result = address_space_validate_range(
		space, request.src_address, size, ADDRESS_TRANSFER_READ | ADDRESS_TRANSFER_USER | ADDRESS_TRANSFER_FAULT_IN);
	if (transfer_result != ADDRESS_TRANSFER_OK) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);

	transfer_result =
		address_space_validate_range(caller_space,
	                                 request.dst_address,
	                                 size,
	                                 ADDRESS_TRANSFER_WRITE | ADDRESS_TRANSFER_USER | ADDRESS_TRANSFER_FAULT_IN);
	if (transfer_result != ADDRESS_TRANSFER_OK) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);

	transfer_result = address_space_copy_between(caller_space, request.dst_address, space, request.src_address, size);
	if (transfer_result != ADDRESS_TRANSFER_OK) return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);

	return syscall_result_ok((uintptr_t)size);
}

static syscall_result_t address_space_copy_to_handler(const struct cap_request* req, struct address_space* space) {
	struct address_space_copy_to_request request;
	size_t                               size;
	syscall_result_t                     copy_result;
	struct address_space*                caller_space;

	copy_result = copy_request_from_caller(req->caller, req->request, req->request_size, &request);
	if (copy_result.status != SYSCALL_STATUS_OK) return copy_result;

	size = request.size;
	if (size == 0u) return syscall_result_ok(0u);

	caller_space = process_address_space(process_lookup(req->caller));
	if (caller_space == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);

	enum address_transfer_result transfer_result;
	transfer_result = address_space_validate_range(
		space, request.dst_address, size, ADDRESS_TRANSFER_WRITE | ADDRESS_TRANSFER_USER | ADDRESS_TRANSFER_FAULT_IN);
	if (transfer_result != ADDRESS_TRANSFER_OK) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);

	transfer_result =
		address_space_validate_range(caller_space,
	                                 request.src_address,
	                                 size,
	                                 ADDRESS_TRANSFER_READ | ADDRESS_TRANSFER_USER | ADDRESS_TRANSFER_FAULT_IN);
	if (transfer_result != ADDRESS_TRANSFER_OK) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);

	transfer_result = address_space_copy_between(space, request.dst_address, caller_space, request.src_address, size);
	if (transfer_result != ADDRESS_TRANSFER_OK) return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);

	return syscall_result_ok((uintptr_t)size);
}

static syscall_result_t address_space_handler(const struct cap_request* req) {
	struct address_space* space;
	enum address_space_op op;
	cap_rights_t          required_rights;

	space = (struct address_space*)(uintptr_t)req->object_id;
	if (space == NULL) return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);

	if (req->request_size < sizeof(struct address_space_request_header)) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	struct address_space_request_header header;
	syscall_result_t copy_result = copy_request_from_caller(req->caller, req->request, req->request_size, &header);
	if (copy_result.status != SYSCALL_STATUS_OK) return copy_result;

	op = header.op;

	switch (op) {
	case ADDRESS_SPACE_OP_RESERVE:
	case ADDRESS_SPACE_OP_RESERVE_AT:
		required_rights = CAP_ALLOCATE;
		break;
	case ADDRESS_SPACE_OP_FREE:
		required_rights = CAP_DESTROY;
		break;
	case ADDRESS_SPACE_OP_MAP:
	case ADDRESS_SPACE_OP_UNMAP:
	case ADDRESS_SPACE_OP_PROTECT:
		required_rights = CAP_MAP;
		break;
	case ADDRESS_SPACE_OP_QUERY:
	case ADDRESS_SPACE_OP_COPY_FROM:
		required_rights = CAP_READ;
		break;
	case ADDRESS_SPACE_OP_COPY_TO:
		required_rights = CAP_WRITE;
		break;
	default:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	if ((req->rights & required_rights) != required_rights) {
		return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	}

	switch (op) {
	case ADDRESS_SPACE_OP_RESERVE:
		return address_space_reserve_handler(req, space);
	case ADDRESS_SPACE_OP_RESERVE_AT:
		return address_space_reserve_at_handler(req, space);
	case ADDRESS_SPACE_OP_FREE:
		return address_space_free_handler(req, space);
	case ADDRESS_SPACE_OP_MAP:
		return address_space_map_handler(req, space);
	case ADDRESS_SPACE_OP_UNMAP:
		return address_space_unmap_handler(req, space);
	case ADDRESS_SPACE_OP_PROTECT:
		return address_space_protect_handler(req, space);
	case ADDRESS_SPACE_OP_QUERY:
		return address_space_query_handler(req, space);
	case ADDRESS_SPACE_OP_COPY_FROM:
		return address_space_copy_from_handler(req, space);
	case ADDRESS_SPACE_OP_COPY_TO:
		return address_space_copy_to_handler(req, space);
	default:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
}

cap_id_t kernel_address_space_grant(struct process* process, cap_rights_t rights) {
	struct cap_object*    object;
	struct capability*    cap;
	struct address_space* space;

	if (process == NULL) return CAP_ID_INVALID;

	space = process_address_space(process);
	if (space == NULL) return CAP_ID_INVALID;

	object = cap_object_create_kernel((uint64_t)(uintptr_t)space, address_space_handler);
	if (object == NULL) return CAP_ID_INVALID;

	cap = cap_create(object->cap_object_id, process_pid(process), rights, NULL);
	if (cap == NULL) {
		cap_object_destroy(object);
		return CAP_ID_INVALID;
	}

	return cap->cap_id;
}
