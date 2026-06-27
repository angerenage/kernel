#include "address_space.h"

#include <base/address_space.h>
#include <base/cap.h>
#include <core/address_transfer.h>
#include <core/capability.h>
#include <core/message.h>
#include <core/process.h>
#include <core/syscall.h>
#include <core/vmm.h>
#include <string.h>

static syscall_result_t copy_request_from_caller(process_id_t caller_pid, const void* req_ptr, size_t req_size,
                                                 void* out_buf, size_t expected_size) {
	struct process*              caller;
	struct address_space*        space;
	enum address_transfer_result result;

	if (req_ptr == NULL || out_buf == NULL || req_size < expected_size) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	caller = process_lookup(caller_pid);
	if (caller == NULL) return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);

	space = process_address_space(caller);
	if (space == NULL) {
		memcpy(out_buf, req_ptr, expected_size);
		return syscall_result_ok(0u);
	}

	result = address_space_copy_from(space, (uintptr_t)req_ptr, out_buf, expected_size);
	if (result != ADDRESS_TRANSFER_OK) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);

	return syscall_result_ok(0u);
}

static syscall_result_t address_space_query_handler(const struct cap_request* req, struct address_space* space) {
	struct address_space_query_request request;
	struct vmm_info                    info;
	struct process*                    caller;
	syscall_result_t                   copy_result;

	copy_result = copy_request_from_caller(req->caller, req->request, req->request_size, &request, sizeof(request));
	if (copy_result.status != SYSCALL_STATUS_OK) return copy_result;
	if (request.id == VMM_ID_INVALID || !vmm_query_id(space, request.id, &info)) {
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}

	caller = process_lookup(req->caller);
	if (caller == NULL) return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	if (message_queue_send(&caller->message_queue, req->caller, &info, sizeof(info)) != MESSAGE_OK) {
		return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	}
	return syscall_result_ok(0u);
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
	syscall_result_t                    copy_result =
		copy_request_from_caller(req->caller, req->request, req->request_size, &header, sizeof(header));
	if (copy_result.status != SYSCALL_STATUS_OK) return copy_result;

	op = header.op;

	switch (op) {
	case ADDRESS_SPACE_OP_QUERY:
		required_rights = CAP_READ;
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
