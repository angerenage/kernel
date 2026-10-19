#include <runtime/terminal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <system/capability.h>

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

syscall_status_t terminal_get_info(cap_id_t terminal_cap, struct terminal_get_info_response* out_info) {
	const struct terminal_get_info_request request = {
		.header = {.op = TERMINAL_OP_GET_INFO},
	};
	size_t response_size = 0u;

	if (terminal_cap == CAP_ID_INVALID || out_info == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	syscall_status_t status =
		cap_call(terminal_cap, &request, sizeof(request), out_info, sizeof(*out_info), &response_size);
	if (status != SYSCALL_STATUS_OK) return status;
	if (response_size != sizeof(*out_info) || out_info->columns == 0u || out_info->rows == 0u)
		return SYSCALL_STATUS_FAILED;
	return SYSCALL_STATUS_OK;
}

syscall_status_t terminal_get_cursor(cap_id_t terminal_cap, struct terminal_get_cursor_response* out_cursor) {
	const struct terminal_get_cursor_request request = {
		.header = {.op = TERMINAL_OP_GET_CURSOR},
	};
	size_t response_size = 0u;

	if (terminal_cap == CAP_ID_INVALID || out_cursor == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	syscall_status_t status =
		cap_call(terminal_cap, &request, sizeof(request), out_cursor, sizeof(*out_cursor), &response_size);
	if (status != SYSCALL_STATUS_OK) return status;
	if (response_size != sizeof(*out_cursor) || out_cursor->line == 0u) return SYSCALL_STATUS_FAILED;
	return SYSCALL_STATUS_OK;
}

syscall_status_t terminal_get_selection(cap_id_t terminal_cap, struct terminal_get_selection_response* out_selection) {
	const struct terminal_get_selection_request request = {
		.header = {.op = TERMINAL_OP_GET_SELECTION},
	};
	size_t response_size = 0u;

	if (terminal_cap == CAP_ID_INVALID || out_selection == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	syscall_status_t status =
		cap_call(terminal_cap, &request, sizeof(request), out_selection, sizeof(*out_selection), &response_size);
	if (status != SYSCALL_STATUS_OK) return status;
	if (response_size != sizeof(*out_selection) || out_selection->anchor.line == 0u ||
	    out_selection->focus.line == 0u ||
	    (out_selection->anchor.line == out_selection->focus.line &&
	     out_selection->anchor.column == out_selection->focus.column))
		return SYSCALL_STATUS_FAILED;
	return SYSCALL_STATUS_OK;
}

syscall_status_t terminal_set_selection(cap_id_t terminal_cap, struct terminal_selection_position anchor,
                                        struct terminal_selection_position focus) {
	const struct terminal_set_selection_request request = {
		.header = {.op = TERMINAL_OP_SET_SELECTION},
		.anchor = anchor,
		.focus  = focus,
	};
	size_t response_size = 0u;

	if (terminal_cap == CAP_ID_INVALID || anchor.line == 0u || focus.line == 0u ||
	    (anchor.line == focus.line && anchor.column == focus.column))
		return SYSCALL_STATUS_BAD_ARGUMENT;
	syscall_status_t status = cap_call(terminal_cap, &request, sizeof(request), NULL, 0u, &response_size);
	if (status != SYSCALL_STATUS_OK) return status;
	return response_size == 0u ? SYSCALL_STATUS_OK : SYSCALL_STATUS_FAILED;
}

syscall_status_t terminal_clear_selection(cap_id_t terminal_cap) {
	const struct terminal_clear_selection_request request = {
		.header = {.op = TERMINAL_OP_CLEAR_SELECTION},
	};
	size_t response_size = 0u;

	if (terminal_cap == CAP_ID_INVALID) return SYSCALL_STATUS_BAD_ARGUMENT;
	syscall_status_t status = cap_call(terminal_cap, &request, sizeof(request), NULL, 0u, &response_size);
	if (status != SYSCALL_STATUS_OK) return status;
	return response_size == 0u ? SYSCALL_STATUS_OK : SYSCALL_STATUS_FAILED;
}

syscall_status_t terminal_get_clipboard(cap_id_t terminal_cap, void* buffer, size_t capacity, size_t* out_size) {
	const struct terminal_get_clipboard_request request = {
		.header = {.op = TERMINAL_OP_GET_CLIPBOARD},
	};
	struct terminal_get_clipboard_response* response;
	size_t                                  response_size = 0u;
	syscall_status_t                        status;

	if (terminal_cap == CAP_ID_INVALID || out_size == NULL || (capacity != 0u && buffer == NULL))
		return SYSCALL_STATUS_BAD_ARGUMENT;
	*out_size = 0u;
	response  = malloc(CAP_MAX_RESPONSE_SIZE);
	if (response == NULL) return SYSCALL_STATUS_FAILED;
	status = cap_call(terminal_cap, &request, sizeof(request), response, CAP_MAX_RESPONSE_SIZE, &response_size);
	if (status != SYSCALL_STATUS_OK) {
		free(response);
		return status;
	}
	if (response_size < sizeof(*response) || response->reserved != 0u || response->size > TERMINAL_CLIPBOARD_CAPACITY ||
	    response_size != sizeof(*response) + response->size || !clipboard_text_valid(response->text, response->size)) {
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

syscall_status_t terminal_set_clipboard(cap_id_t terminal_cap, const void* text, size_t size) {
	struct terminal_set_clipboard_request* request;
	size_t                                 request_size;
	size_t                                 response_size = 0u;
	syscall_status_t                       status;

	if (terminal_cap == CAP_ID_INVALID || size > TERMINAL_CLIPBOARD_CAPACITY || !clipboard_text_valid(text, size))
		return SYSCALL_STATUS_BAD_ARGUMENT;
	request_size = sizeof(*request) + size;
	request      = malloc(request_size);
	if (request == NULL) return SYSCALL_STATUS_FAILED;
	request->header.op = TERMINAL_OP_SET_CLIPBOARD;
	request->size      = (uint32_t)size;
	request->reserved  = 0u;
	if (size != 0u) memcpy(request->text, text, size);
	status = cap_call(terminal_cap, request, request_size, NULL, 0u, &response_size);
	free(request);
	if (status != SYSCALL_STATUS_OK) return status;
	return response_size == 0u ? SYSCALL_STATUS_OK : SYSCALL_STATUS_FAILED;
}

syscall_status_t terminal_acquire_interrupt_signal(cap_id_t terminal_cap, cap_id_t* out_signal_cap) {
	const struct terminal_acquire_interrupt_signal_request request = {
		.header = {.op = TERMINAL_OP_ACQUIRE_INTERRUPT_SIGNAL},
	};
	struct terminal_acquire_interrupt_signal_response response      = {.capability = CAP_ID_INVALID};
	size_t                                            response_size = 0u;

	if (terminal_cap == CAP_ID_INVALID || out_signal_cap == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	*out_signal_cap = CAP_ID_INVALID;
	syscall_status_t status =
		cap_call(terminal_cap, &request, sizeof(request), &response, sizeof(response), &response_size);
	if (status != SYSCALL_STATUS_OK) return status;
	if (response_size != sizeof(response) || response.capability == CAP_ID_INVALID) {
		if (response.capability != CAP_ID_INVALID) (void)cap_drop(response.capability);
		return SYSCALL_STATUS_FAILED;
	}
	*out_signal_cap = response.capability;
	return SYSCALL_STATUS_OK;
}

syscall_status_t terminal_discard_input(cap_id_t terminal_cap, uint64_t* out_interrupt_generation) {
	const struct terminal_discard_input_request request = {
		.header = {.op = TERMINAL_OP_DISCARD_INPUT},
	};
	struct terminal_discard_input_response response;
	size_t                                 response_size = 0u;

	if (terminal_cap == CAP_ID_INVALID || out_interrupt_generation == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	syscall_status_t status =
		cap_call(terminal_cap, &request, sizeof(request), &response, sizeof(response), &response_size);
	if (status != SYSCALL_STATUS_OK) return status;
	if (response_size != sizeof(response)) return SYSCALL_STATUS_FAILED;
	*out_interrupt_generation = response.interrupt_generation;
	return SYSCALL_STATUS_OK;
}
