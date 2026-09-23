#pragma once

#include <base/cap.h>
#include <stdbool.h>
#include <stdint.h>

/* Opaque token naming one firmware-discovered, claimable fixed interrupt source. */
typedef uint64_t interrupt_source_t;
#define INTERRUPT_SOURCE_INVALID UINT64_MAX

/*
 * Opaque, non-authoritative token naming a kernel-managed message context.
 * Allocation authority comes from CAP_ALLOCATE on the Interrupts resource.
 */
typedef uint64_t interrupt_message_context_t;
#define INTERRUPT_MESSAGE_CONTEXT_INVALID UINT64_MAX

/* Interrupt delivery mechanism selected by the kernel. */
enum interrupt_kind {
	INTERRUPT_KIND_SOURCE = 0,
	INTERRUPT_KIND_MESSAGE,
};

/* Result returned by interrupt-core operations. */
enum interrupt_result {
	INTERRUPT_OK = 0,
	INTERRUPT_INVALID_ARGUMENTS,
	INTERRUPT_NOT_FOUND,
	INTERRUPT_ALREADY_CLAIMED,
	INTERRUPT_ALREADY_BOUND,
	INTERRUPT_NOT_BOUND,
	INTERRUPT_UNAVAILABLE,
	INTERRUPT_NO_MEMORY,
	INTERRUPT_FAILED,
};

/* Public state that deliberately excludes all hardware delivery identities. */
struct interrupt_info {
	enum interrupt_kind kind;
	bool                bound;
};

/* Device-programmable message values returned for a message interrupt. */
struct interrupt_message {
	uint64_t message_address;
	uint32_t message_data;
};

/* Operations accepted by an Interrupt capability. */
enum interrupt_op {
	INTERRUPT_OP_INFO = 0,
	INTERRUPT_OP_BIND,
	INTERRUPT_OP_UNBIND,
	INTERRUPT_OP_DESTROY,
};

/* Common header for Interrupt capability requests. */
struct interrupt_request_header {
	enum interrupt_op op;
};

/* Request to bind an Interrupt to a Signal capability. */
struct interrupt_bind_request {
	struct interrupt_request_header header;
	cap_id_t                        signal_cap;
};

/* Empty request used for info, unbind, and destroy operations. */
struct interrupt_simple_request {
	struct interrupt_request_header header;
};

/* Response returned by INTERRUPT_OP_INFO; kernel producers must zero-initialize it. */
struct interrupt_info_response {
	struct interrupt_info info;
};

/* Operations accepted by the kernel Interrupts resource. */
enum interrupts_op {
	INTERRUPTS_OP_CLAIM_SOURCE = 0,
	INTERRUPTS_OP_ALLOCATE_MESSAGE,
};

/* Common header for Interrupts resource requests. */
struct interrupts_request_header {
	enum interrupts_op op;
};

/* Request to claim one fixed hardware source. */
struct interrupts_claim_source_request {
	struct interrupts_request_header header;
	interrupt_source_t               source;
};

/* Request to allocate one message-signaled interrupt. */
struct interrupts_allocate_message_request {
	struct interrupts_request_header header;
	interrupt_message_context_t      context;
};

/* Response returned after claiming a fixed source; kernel producers must zero-initialize it. */
struct interrupts_claim_source_response {
	cap_id_t interrupt_cap;
};

/* Response returned after allocating a message interrupt; kernel producers must zero-initialize it. */
struct interrupts_allocate_message_response {
	cap_id_t interrupt_cap;
	uint64_t message_address;
	uint32_t message_data;
};
