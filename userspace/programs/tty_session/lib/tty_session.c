#include <protocol/tty_session.h>
#include <runtime/registry.h>
#include <runtime/tty_session.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
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

/* Validate length-delimited UTF-8 plain text accepted by the terminal clipboard. */
static bool clipboard_text_valid(const uint8_t* text, size_t size) {
	size_t offset = 0u;

	if (size != 0u && text == NULL) return false;
	while (offset < size) {
		uint8_t  first = text[offset];
		uint32_t value;
		size_t   length;

		if (first < 0x80u) {
			value  = first;
			length = 1u;
		}
		else if (first >= 0xc2u && first <= 0xdfu) {
			value  = first & 0x1fu;
			length = 2u;
		}
		else if (first >= 0xe0u && first <= 0xefu) {
			value  = first & 0x0fu;
			length = 3u;
		}
		else if (first >= 0xf0u && first <= 0xf4u) {
			value  = first & 0x07u;
			length = 4u;
		}
		else return false;
		if (length > size - offset) return false;
		for (size_t i = 1u; i < length; i++) {
			if ((text[offset + i] & 0xc0u) != 0x80u) return false;
			value = (value << 6u) | (text[offset + i] & 0x3fu);
		}
		uint32_t minimum = length == 1u ? 0u : length == 2u ? 0x80u : length == 3u ? 0x800u : 0x10000u;
		if (value < minimum || value > 0x10ffffu || (value >= 0xd800u && value <= 0xdfffu) ||
		    (value < 0x20u && value != '\t' && value != '\n' && value != '\r') || (value >= 0x7fu && value <= 0x9fu))
			return false;
		offset += length;
	}
	return true;
}

struct registry_call_result tty_session_acquire(struct registry_service_handle* out_handle) {
	static const struct registry_protocol_query query = {
		.namespace_path = TTY_SESSION_NAMESPACE,
		.protocol       = TTY_SESSION_PROTOCOL_NAME,
		.major          = TTY_SESSION_PROTOCOL_VERSION_MAJOR,
		.minor          = TTY_SESSION_PROTOCOL_VERSION_MINOR,
	};
	return registry_acquire(&query, TTY_SESSION_SERVICE_NAME, out_handle);
}

syscall_status_t tty_session_get_info(cap_id_t session_cap, struct tty_session_get_info_response* out_info) {
	const struct tty_session_get_info_request request = {
		.header = {.op = TTY_SESSION_OP_GET_INFO},
	};
	syscall_status_t status;

	if (out_info == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	status = fixed_call(session_cap, &request, sizeof(request), out_info, sizeof(*out_info));
	if (status != SYSCALL_STATUS_OK) return status;
	if (out_info->columns == 0u || out_info->rows == 0u || out_info->session_pid == PROCESS_PID_INVALID)
		return SYSCALL_STATUS_FAILED;
	return SYSCALL_STATUS_OK;
}

syscall_status_t tty_session_get_cursor(cap_id_t session_cap, struct tty_session_get_cursor_response* out_cursor) {
	const struct tty_session_get_cursor_request request = {
		.header = {.op = TTY_SESSION_OP_GET_CURSOR},
	};

	if (out_cursor == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	syscall_status_t status = fixed_call(session_cap, &request, sizeof(request), out_cursor, sizeof(*out_cursor));
	if (status != SYSCALL_STATUS_OK) return status;
	return out_cursor->line != 0u ? SYSCALL_STATUS_OK : SYSCALL_STATUS_FAILED;
}

syscall_status_t tty_session_get_selection(cap_id_t                                   session_cap,
                                           struct tty_session_get_selection_response* out_selection) {
	const struct tty_session_get_selection_request request = {
		.header = {.op = TTY_SESSION_OP_GET_SELECTION},
	};
	syscall_status_t status;

	if (out_selection == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	status = fixed_call(session_cap, &request, sizeof(request), out_selection, sizeof(*out_selection));
	if (status != SYSCALL_STATUS_OK) return status;
	if (out_selection->anchor.line == 0u || out_selection->focus.line == 0u ||
	    (out_selection->anchor.line == out_selection->focus.line &&
	     out_selection->anchor.column == out_selection->focus.column))
		return SYSCALL_STATUS_FAILED;
	return SYSCALL_STATUS_OK;
}

syscall_status_t tty_session_set_selection(cap_id_t session_cap, struct tty_session_selection_position anchor,
                                           struct tty_session_selection_position focus) {
	const struct tty_session_set_selection_request request = {
		.header = {.op = TTY_SESSION_OP_SET_SELECTION},
		.anchor = anchor,
		.focus  = focus,
	};

	if (anchor.line == 0u || focus.line == 0u || (anchor.line == focus.line && anchor.column == focus.column))
		return SYSCALL_STATUS_BAD_ARGUMENT;
	return fixed_call(session_cap, &request, sizeof(request), NULL, 0u);
}

syscall_status_t tty_session_clear_selection(cap_id_t session_cap) {
	const struct tty_session_clear_selection_request request = {
		.header = {.op = TTY_SESSION_OP_CLEAR_SELECTION},
	};
	return fixed_call(session_cap, &request, sizeof(request), NULL, 0u);
}

syscall_status_t tty_session_get_clipboard(cap_id_t session_cap, void* buffer, size_t capacity, size_t* out_size) {
	const struct tty_session_get_clipboard_request request = {
		.header = {.op = TTY_SESSION_OP_GET_CLIPBOARD},
	};
	struct tty_session_get_clipboard_response* response;
	size_t                                     response_size = 0u;
	syscall_status_t                           status;

	if (session_cap == CAP_ID_INVALID || out_size == NULL || (capacity != 0u && buffer == NULL))
		return SYSCALL_STATUS_BAD_ARGUMENT;
	*out_size = 0u;
	response  = malloc(CAP_MAX_RESPONSE_SIZE);
	if (response == NULL) return SYSCALL_STATUS_FAILED;
	status = cap_call(session_cap, &request, sizeof(request), response, CAP_MAX_RESPONSE_SIZE, &response_size);
	if (status != SYSCALL_STATUS_OK) {
		free(response);
		return status;
	}
	if (response_size < sizeof(*response) || response->reserved != 0u ||
	    response->size > TTY_SESSION_CLIPBOARD_CAPACITY || response_size != sizeof(*response) + response->size ||
	    !clipboard_text_valid(response->text, response->size)) {
		free(response);
		return SYSCALL_STATUS_FAILED;
	}
	*out_size = response->size;
	if (buffer == NULL && capacity == 0u) {
		free(response);
		return SYSCALL_STATUS_OK;
	}
	if (capacity < response->size) {
		free(response);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if (response->size != 0u) memcpy(buffer, response->text, response->size);
	free(response);
	return SYSCALL_STATUS_OK;
}

syscall_status_t tty_session_set_clipboard(cap_id_t session_cap, const void* text, size_t size) {
	struct tty_session_set_clipboard_request* request;
	size_t                                    request_size;
	size_t                                    response_size = 0u;
	syscall_status_t                          status;

	if (session_cap == CAP_ID_INVALID || size > TTY_SESSION_CLIPBOARD_CAPACITY || !clipboard_text_valid(text, size))
		return SYSCALL_STATUS_BAD_ARGUMENT;
	request_size = sizeof(*request) + size;
	request      = malloc(request_size);
	if (request == NULL) return SYSCALL_STATUS_FAILED;
	request->header.op = TTY_SESSION_OP_SET_CLIPBOARD;
	request->size      = (uint32_t)size;
	request->reserved  = 0u;
	if (size != 0u) memcpy(request->text, text, size);
	status = cap_call(session_cap, request, request_size, NULL, 0u, &response_size);
	free(request);
	if (status != SYSCALL_STATUS_OK) return status;
	return response_size == 0u ? SYSCALL_STATUS_OK : SYSCALL_STATUS_FAILED;
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
	status = cap_delegate(process_cap, session_owner, CAP_CALL | CAP_READ | CAP_DESTROY, &request.process_cap);
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
