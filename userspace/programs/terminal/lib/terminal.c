#include <runtime/terminal.h>
#include <stddef.h>
#include <system/capability.h>

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
