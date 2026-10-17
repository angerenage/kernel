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
