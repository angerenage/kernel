#include <base/dma.h>
#include <runtime/diagnostic.h>
#include <system/capability.h>
#include <system/dma.h>

static syscall_status_t fixed_call(cap_id_t cap, const void* request, size_t request_size, void* response,
                                   size_t response_size, uint64_t operation __attribute__((unused))) {
	if (cap == CAP_ID_INVALID || request == NULL || (response_size != 0u && response == NULL))
		return SYSCALL_STATUS_BAD_ARGUMENT;
	syscall_result_t result = cap_call_syscall(cap, request, request_size, response, response_size);
	RUNTIME_DIAGNOSTIC_OPERATION_RESULT(operation, result);
	if (result.status != SYSCALL_STATUS_OK) return result.status;
	return result.value == response_size ? SYSCALL_STATUS_OK : SYSCALL_STATUS_FAILED;
}

syscall_status_t dma_resolve_source(cap_id_t dma_cap, uint64_t controller_register_address, uint32_t local_source_id,
                                    dma_source_t* out_source) {
	const struct dma_resolve_source_request request = {
		.header                      = {.op = DMA_OP_RESOLVE_SOURCE},
		.controller_register_address = controller_register_address,
		.local_source_id             = local_source_id,
		.reserved                    = 0u,
	};
	struct dma_resolve_source_response response = {.source = DMA_SOURCE_INVALID};
	if (out_source == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	*out_source = DMA_SOURCE_INVALID;
	syscall_status_t status =
		fixed_call(dma_cap, &request, sizeof(request), &response, sizeof(response), DMA_OP_RESOLVE_SOURCE);
	if (status == SYSCALL_STATUS_OK) *out_source = response.source;
	return status;
}

syscall_status_t dma_create_address_space(cap_id_t dma_cap, dma_source_t source, cap_id_t* out_address_space_cap) {
	const struct dma_create_address_space_request request = {
		.header = {.op = DMA_OP_CREATE_ADDRESS_SPACE},
		.source = source,
	};
	struct dma_create_address_space_response response = {.address_space_cap = CAP_ID_INVALID};
	if (out_address_space_cap == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	*out_address_space_cap = CAP_ID_INVALID;
	syscall_status_t status =
		fixed_call(dma_cap, &request, sizeof(request), &response, sizeof(response), DMA_OP_CREATE_ADDRESS_SPACE);
	if (status == SYSCALL_STATUS_OK) *out_address_space_cap = response.address_space_cap;
	return status;
}

syscall_status_t dma_bind(cap_id_t dma_cap, dma_source_t source, cap_id_t address_space_cap,
                          cap_id_t* out_binding_cap) {
	const struct dma_bind_request request = {
		.header            = {.op = DMA_OP_BIND},
		.source            = source,
		.address_space_cap = address_space_cap,
	};
	struct dma_bind_response response = {.binding_cap = CAP_ID_INVALID};
	if (out_binding_cap == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	*out_binding_cap        = CAP_ID_INVALID;
	syscall_status_t status = fixed_call(dma_cap, &request, sizeof(request), &response, sizeof(response), DMA_OP_BIND);
	if (status == SYSCALL_STATUS_OK) *out_binding_cap = response.binding_cap;
	return status;
}

syscall_status_t dma_recover(cap_id_t dma_cap, dma_source_t source, cap_id_t* out_binding_cap) {
	const struct dma_recover_request request = {
		.header = {.op = DMA_OP_RECOVER},
		.source = source,
	};
	struct dma_recover_response response = {.binding_cap = CAP_ID_INVALID};
	if (out_binding_cap == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	*out_binding_cap = CAP_ID_INVALID;
	syscall_status_t status =
		fixed_call(dma_cap, &request, sizeof(request), &response, sizeof(response), DMA_OP_RECOVER);
	if (status == SYSCALL_STATUS_OK) *out_binding_cap = response.binding_cap;
	return status;
}

syscall_status_t dma_binding_unbind(cap_id_t binding_cap) {
	const struct dma_binding_unbind_request request = {.header = {.op = DMA_BINDING_OP_UNBIND}};
	return fixed_call(binding_cap, &request, sizeof(request), NULL, 0u, DMA_BINDING_OP_UNBIND);
}
