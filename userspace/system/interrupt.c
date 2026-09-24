#include <base/interrupt.h>
#include <runtime/diagnostic.h>
#include <string.h>
#include <system/capability.h>
#include <system/interrupt.h>

static syscall_status_t fixed_call(cap_id_t cap, const void* request, size_t request_size, void* response,
                                   size_t response_size, uint64_t operation) {
	(void)operation;
	if (cap == CAP_ID_INVALID || request == NULL || (response_size != 0u && response == NULL))
		return SYSCALL_STATUS_BAD_ARGUMENT;
	syscall_result_t result = cap_call_syscall(cap, request, request_size, response, response_size);
	RUNTIME_DIAGNOSTIC_OPERATION_RESULT(operation, result);
	if (result.status != SYSCALL_STATUS_OK) return result.status;
	return result.value == response_size ? SYSCALL_STATUS_OK : SYSCALL_STATUS_FAILED;
}

syscall_status_t interrupts_claim_source(cap_id_t interrupts_cap, interrupt_source_t source,
                                         cap_id_t* out_interrupt_cap) {
	const struct interrupts_claim_source_request request = {
		.header = {.op = INTERRUPTS_OP_CLAIM_SOURCE},
		.source = source,
	};
	struct interrupts_claim_source_response response = {0};
	if (out_interrupt_cap == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	*out_interrupt_cap = CAP_ID_INVALID;
	if (source == INTERRUPT_SOURCE_INVALID) return SYSCALL_STATUS_BAD_ARGUMENT;
	syscall_status_t status =
		fixed_call(interrupts_cap, &request, sizeof(request), &response, sizeof(response), INTERRUPTS_OP_CLAIM_SOURCE);
	if (status == SYSCALL_STATUS_OK) {
		if (response.interrupt_cap == CAP_ID_INVALID) return SYSCALL_STATUS_FAILED;
		*out_interrupt_cap = response.interrupt_cap;
	}
	return status;
}

syscall_status_t interrupts_allocate_message(cap_id_t interrupts_cap, interrupt_message_context_t context,
                                             cap_id_t* out_interrupt_cap, struct interrupt_message* out_message) {
	const struct interrupts_allocate_message_request request = {
		.header  = {.op = INTERRUPTS_OP_ALLOCATE_MESSAGE},
		.context = context,
	};
	struct interrupts_allocate_message_response response = {0};
	if (out_interrupt_cap == NULL || out_message == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	*out_interrupt_cap = CAP_ID_INVALID;
	memset(out_message, 0, sizeof(*out_message));
	if (context == INTERRUPT_MESSAGE_CONTEXT_INVALID) return SYSCALL_STATUS_BAD_ARGUMENT;
	syscall_status_t status = fixed_call(
		interrupts_cap, &request, sizeof(request), &response, sizeof(response), INTERRUPTS_OP_ALLOCATE_MESSAGE);
	if (status == SYSCALL_STATUS_OK) {
		if (response.interrupt_cap == CAP_ID_INVALID) return SYSCALL_STATUS_FAILED;
		*out_interrupt_cap           = response.interrupt_cap;
		out_message->message_address = response.message_address;
		out_message->message_data    = response.message_data;
	}
	return status;
}

syscall_status_t interrupt_info(cap_id_t interrupt_cap, struct interrupt_info* out_info) {
	const struct interrupt_simple_request request  = {.header = {.op = INTERRUPT_OP_INFO}};
	struct interrupt_info_response        response = {0};
	if (out_info == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	memset(out_info, 0, sizeof(*out_info));
	syscall_status_t status =
		fixed_call(interrupt_cap, &request, sizeof(request), &response, sizeof(response), INTERRUPT_OP_INFO);
	if (status == SYSCALL_STATUS_OK) *out_info = response.info;
	return status;
}

syscall_status_t interrupt_bind(cap_id_t interrupt_cap, cap_id_t signal_cap) {
	const struct interrupt_bind_request request = {
		.header     = {.op = INTERRUPT_OP_BIND},
		.signal_cap = signal_cap,
	};
	if (signal_cap == CAP_ID_INVALID) return SYSCALL_STATUS_BAD_ARGUMENT;
	return fixed_call(interrupt_cap, &request, sizeof(request), NULL, 0u, INTERRUPT_OP_BIND);
}

syscall_status_t interrupt_unbind(cap_id_t interrupt_cap) {
	const struct interrupt_simple_request request = {.header = {.op = INTERRUPT_OP_UNBIND}};
	return fixed_call(interrupt_cap, &request, sizeof(request), NULL, 0u, INTERRUPT_OP_UNBIND);
}

syscall_status_t interrupt_destroy(cap_id_t interrupt_cap) {
	const struct interrupt_simple_request request = {.header = {.op = INTERRUPT_OP_DESTROY}};
	return fixed_call(interrupt_cap, &request, sizeof(request), NULL, 0u, INTERRUPT_OP_DESTROY);
}
