#include <protocol/tty_session.h>
#include <runtime/init.h>
#include <runtime/tty_session.h>
#include <stddef.h>
#include <system/capability.h>

#define TTY_SESSION_PROTOCOL_VERSION_MAJOR 1u
#define TTY_SESSION_PROTOCOL_VERSION_MINOR 0u

static syscall_status_t fixed_call(cap_id_t session_cap, const void* request, size_t request_size, void* response,
                                   size_t response_size) {
	size_t           actual_size = 0u;
	syscall_status_t status;

	if (session_cap == CAP_ID_INVALID || request == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	status = cap_call(session_cap, request, request_size, response, response_size, &actual_size);
	if (status != SYSCALL_STATUS_OK) return status;
	if (actual_size != response_size) return SYSCALL_STATUS_FAILED;
	return SYSCALL_STATUS_OK;
}

struct init_call_result tty_session_acquire(struct init_service_handle* out_handle) {
	static const struct init_protocol_query query = {
		.namespace_path = TTY_SESSION_NAMESPACE,
		.protocol       = TTY_SESSION_PROTOCOL_NAME,
		.major          = TTY_SESSION_PROTOCOL_VERSION_MAJOR,
		.minor          = TTY_SESSION_PROTOCOL_VERSION_MINOR,
	};
	return init_acquire(&query, TTY_SESSION_SERVICE_NAME, out_handle);
}

syscall_status_t tty_session_get_info(cap_id_t session_cap, struct tty_session_get_info_response* out_info) {
	const struct tty_session_get_info_request request = {
		.header = {.op = TTY_SESSION_OP_GET_INFO},
	};
	syscall_status_t status;

	if (out_info == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	status = fixed_call(session_cap, &request, sizeof(request), out_info, sizeof(*out_info));
	if (status != SYSCALL_STATUS_OK) return status;
	if (out_info->columns == 0u || out_info->rows == 0u) return SYSCALL_STATUS_FAILED;
	return SYSCALL_STATUS_OK;
}

syscall_status_t tty_session_get_cursor(cap_id_t session_cap, struct tty_session_get_cursor_response* out_cursor) {
	const struct tty_session_get_cursor_request request = {
		.header = {.op = TTY_SESSION_OP_GET_CURSOR},
	};

	if (out_cursor == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	return fixed_call(session_cap, &request, sizeof(request), out_cursor, sizeof(*out_cursor));
}

syscall_status_t tty_session_claim(cap_id_t session_cap) {
	const struct tty_session_claim_request request = {
		.header = {.op = TTY_SESSION_OP_CLAIM},
	};
	return fixed_call(session_cap, &request, sizeof(request), NULL, 0u);
}

syscall_status_t tty_session_begin_foreground(cap_id_t session_cap, process_id_t process_id) {
	const struct tty_session_begin_foreground_request request = {
		.header     = {.op = TTY_SESSION_OP_BEGIN_FOREGROUND},
		.process_id = process_id,
	};

	if (process_id == PROCESS_PID_INVALID) return SYSCALL_STATUS_BAD_ARGUMENT;
	return fixed_call(session_cap, &request, sizeof(request), NULL, 0u);
}

syscall_status_t tty_session_attach_foreground(cap_id_t session_cap, process_id_t session_owner, cap_id_t process_cap,
                                               process_id_t process_id) {
	struct tty_session_attach_foreground_request request = {
		.header     = {.op = TTY_SESSION_OP_ATTACH_FOREGROUND},
		.process_id = process_id,
	};
	syscall_status_t status;

	if (session_cap == CAP_ID_INVALID || session_owner == PROCESS_PID_INVALID || process_cap == CAP_ID_INVALID ||
	    process_id == PROCESS_PID_INVALID)
		return SYSCALL_STATUS_BAD_ARGUMENT;
	status = cap_delegate(process_cap, session_owner, CAP_CALL | CAP_READ | CAP_MANAGE, &request.process_cap);
	if (status != SYSCALL_STATUS_OK) return status;
	status = fixed_call(session_cap, &request, sizeof(request), NULL, 0u);
	if (status != SYSCALL_STATUS_OK) (void)cap_revoke(request.process_cap, 0u);
	return status;
}

syscall_status_t tty_session_restore_foreground(cap_id_t session_cap) {
	const struct tty_session_restore_foreground_request request = {
		.header = {.op = TTY_SESSION_OP_RESTORE_FOREGROUND},
	};
	return fixed_call(session_cap, &request, sizeof(request), NULL, 0u);
}
