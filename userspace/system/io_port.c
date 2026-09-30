#include <base/io_port.h>
#include <runtime/diagnostic.h>
#include <system/capability.h>
#include <system/io_port.h>

static syscall_status_t fixed_call(cap_id_t cap, const void* request, size_t request_size, void* response,
                                   size_t response_size, enum io_port_op operation) {
	(void)operation;
	if (cap == CAP_ID_INVALID || request == NULL || (response_size != 0u && response == NULL))
		return SYSCALL_STATUS_BAD_ARGUMENT;
	syscall_result_t result = cap_call_syscall(cap, request, request_size, response, response_size);
	RUNTIME_DIAGNOSTIC_OPERATION_RESULT(operation, result);
	if (result.status != SYSCALL_STATUS_OK) return result.status;
	return result.value == response_size ? SYSCALL_STATUS_OK : SYSCALL_STATUS_FAILED;
}

syscall_status_t io_port_info(cap_id_t io_port_cap, struct io_port_info* out_info) {
	const struct io_port_simple_request request = {.header = {.op = IO_PORT_OP_INFO}};
	return fixed_call(io_port_cap, &request, sizeof(request), out_info, sizeof(*out_info), IO_PORT_OP_INFO);
}

syscall_status_t io_port_derive(cap_id_t io_port_cap, uint32_t offset, uint32_t count, cap_rights_t rights,
                                cap_id_t* out_io_port_cap) {
	const struct io_port_derive_request request = {
		.header = {.op = IO_PORT_OP_DERIVE},
		.offset = offset,
		.count  = count,
		.rights = rights,
	};
	struct io_port_derive_response response;
	if (out_io_port_cap == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	*out_io_port_cap = CAP_ID_INVALID;
	syscall_status_t status =
		fixed_call(io_port_cap, &request, sizeof(request), &response, sizeof(response), IO_PORT_OP_DERIVE);
	if (status == SYSCALL_STATUS_OK) *out_io_port_cap = response.io_port_cap;
	return status;
}

syscall_status_t io_port_map(cap_id_t io_port_cap) {
	const struct io_port_simple_request request = {.header = {.op = IO_PORT_OP_MAP}};
	return fixed_call(io_port_cap, &request, sizeof(request), NULL, 0u, IO_PORT_OP_MAP);
}

syscall_status_t io_port_unmap(cap_id_t io_port_cap) {
	const struct io_port_simple_request request = {.header = {.op = IO_PORT_OP_UNMAP}};
	return fixed_call(io_port_cap, &request, sizeof(request), NULL, 0u, IO_PORT_OP_UNMAP);
}

syscall_status_t io_port_read(cap_id_t io_port_cap, uint32_t offset, enum io_port_width width, uint32_t* out_value) {
	const struct io_port_read_request request = {
		.header = {.op = IO_PORT_OP_READ},
		.offset = offset,
		.width  = width,
	};
	struct io_port_read_response response;
	if (out_value == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	syscall_status_t status =
		fixed_call(io_port_cap, &request, sizeof(request), &response, sizeof(response), IO_PORT_OP_READ);
	if (status == SYSCALL_STATUS_OK) *out_value = response.value;
	return status;
}

syscall_status_t io_port_write(cap_id_t io_port_cap, uint32_t offset, enum io_port_width width, uint32_t value) {
	const struct io_port_write_request request = {
		.header = {.op = IO_PORT_OP_WRITE},
		.offset = offset,
		.width  = width,
		.value  = value,
	};
	return fixed_call(io_port_cap, &request, sizeof(request), NULL, 0u, IO_PORT_OP_WRITE);
}
