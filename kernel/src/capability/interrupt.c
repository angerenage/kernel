#include "interrupt.h"

#include <base/interrupt.h>
#include <base/syscall.h>
#include <core/capability.h>
#include <core/interrupt.h>
#include <core/signal.h>
#include <kernel/capability.h>
#include <libc/stdlib.h>
#include <string.h>

#include "signal.h"

#define INTERRUPTS_RESOURCE_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_READ | CAP_MANAGE | CAP_ALLOCATE | CAP_DELEGATE))
#define INTERRUPT_CAP_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_READ | CAP_MANAGE | CAP_DESTROY | CAP_DELEGATE))

struct kernel_interrupt {
	struct interrupt* interrupt;
	cap_object_id_t   cap_object_id;
	bool              destroyed;
};

static cap_object_id_t interrupts_resource_object_id = CAP_OBJECT_ID_INVALID;

#if defined(KERNEL_CAPABILITY_INTERRUPT_TEST)
static bool     fail_next_interrupt_response;
static cap_id_t last_rollback_cap = CAP_ID_INVALID;

void kernel_capability_interrupt_test_fail_next_response(void) {
	fail_next_interrupt_response = true;
	last_rollback_cap            = CAP_ID_INVALID;
}

cap_id_t kernel_capability_interrupt_test_last_rollback_cap(void) {
	return last_rollback_cap;
}
#endif

static syscall_result_t kernel_interrupt_handler(const struct cap_request* req);

static syscall_result_t interrupt_result_to_syscall(enum interrupt_result result) {
	switch (result) {
	case INTERRUPT_OK:
		return syscall_result_ok(0u);
	case INTERRUPT_INVALID_ARGUMENTS:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, (uintptr_t)result);
	case INTERRUPT_NOT_FOUND:
	case INTERRUPT_ALREADY_CLAIMED:
	case INTERRUPT_ALREADY_BOUND:
	case INTERRUPT_UNAVAILABLE:
		return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, (uintptr_t)result);
	case INTERRUPT_NOT_BOUND:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, (uintptr_t)result);
	case INTERRUPT_NO_MEMORY:
	case INTERRUPT_FAILED:
	default:
		return syscall_result_error(SYSCALL_STATUS_FAILED, (uintptr_t)result);
	}
}

static bool copy_request(const struct cap_request* req, void* out, size_t size) {
	if (req == NULL || req->request == NULL || out == NULL || req->request_size != size) return false;
	memcpy(out, req->request, size);
	return true;
}

static syscall_result_t write_published_response(const struct cap_request* req, const void* response,
                                                 size_t response_size, cap_id_t published_cap) {
#if defined(KERNEL_CAPABILITY_INTERRUPT_TEST)
	if (fail_next_interrupt_response) {
		fail_next_interrupt_response = false;
		last_rollback_cap            = published_cap;
		return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	}
#else
	(void)published_cap;
#endif
	return cap_kernel_write_response(req, response, response_size);
}

static enum interrupt_result kernel_interrupt_destroy(struct kernel_interrupt* managed) {
	enum interrupt_result result;
	if (managed == NULL || __atomic_load_n(&managed->destroyed, __ATOMIC_ACQUIRE)) return INTERRUPT_UNAVAILABLE;
	result = interrupt_destroy(managed->interrupt);
	if (result == INTERRUPT_OK) __atomic_store_n(&managed->destroyed, true, __ATOMIC_RELEASE);
	return result;
}

static void kernel_interrupt_object_destroy(uint64_t object_id) {
	struct kernel_interrupt* managed = (struct kernel_interrupt*)(uintptr_t)object_id;
	if (managed == NULL) return;
	if (!__atomic_load_n(&managed->destroyed, __ATOMIC_ACQUIRE)) return;
	interrupt_release(managed->interrupt);
	free(managed);
}

static void kernel_interrupt_unpublish(struct kernel_interrupt* managed) {
	if (kernel_interrupt_destroy(managed) == INTERRUPT_OK) (void)cap_object_destroy_with_id(managed->cap_object_id);
}

static void kernel_interrupt_object_event(struct cap_object* object, enum cap_object_event event) {
	struct kernel_interrupt* managed;
	if (event != CAP_OBJECT_EVENT_ZERO_GRANTS || object == NULL) return;
	managed = (struct kernel_interrupt*)(uintptr_t)object->object_id;
	if (kernel_interrupt_destroy(managed) != INTERRUPT_OK) return;
	(void)cap_object_destroy_if_unused(object);
}

static cap_id_t kernel_interrupt_publish(struct interrupt* interrupt, process_id_t recipient) {
	struct kernel_interrupt* managed;
	cap_object_id_t          object_id;
	cap_id_t                 cap;
	bool                     created = false;

	if (interrupt == NULL || recipient == PROCESS_PID_INVALID) return CAP_ID_INVALID;
	managed = calloc(1u, sizeof(*managed));
	if (managed == NULL || !interrupt_retain(interrupt)) {
		free(managed);
		(void)interrupt_destroy(interrupt);
		return CAP_ID_INVALID;
	}
	managed->interrupt = interrupt;
	object_id          = cap_object_create_kernel_lifecycle((uint64_t)(uintptr_t)managed,
	                                                        kernel_interrupt_handler,
	                                                        NULL,
	                                                        kernel_interrupt_object_destroy,
	                                                        kernel_interrupt_object_event,
	                                                        &created);
	if (object_id == CAP_OBJECT_ID_INVALID) {
		interrupt_release(interrupt);
		free(managed);
		(void)interrupt_destroy(interrupt);
		return CAP_ID_INVALID;
	}
	if (!created) {
		interrupt_release(interrupt);
		free(managed);
		(void)interrupt_destroy(interrupt);
		return CAP_ID_INVALID;
	}
	managed->cap_object_id = object_id;
	cap                    = cap_create(object_id, recipient, INTERRUPT_CAP_RIGHTS, NULL);
	if (cap == CAP_ID_INVALID) {
		kernel_interrupt_unpublish(managed);
		return CAP_ID_INVALID;
	}
	return cap;
}

static syscall_result_t kernel_interrupt_info_handler(const struct cap_request* req, struct kernel_interrupt* managed) {
	struct interrupt_simple_request request;
	struct interrupt_info_response  response = {0};
	if ((req->rights & CAP_READ) == 0u) return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	if (!copy_request(req, &request, sizeof(request)) || !cap_kernel_response_fits(req, sizeof(response)))
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	enum interrupt_result result = interrupt_get_info(managed->interrupt, &response.info);
	if (result != INTERRUPT_OK) return interrupt_result_to_syscall(result);
	return cap_kernel_write_response(req, &response, sizeof(response));
}

static syscall_result_t kernel_interrupt_bind_handler(const struct cap_request* req, struct kernel_interrupt* managed) {
	struct interrupt_bind_request request;
	struct signal*                signal;
	if ((req->rights & CAP_MANAGE) == 0u) return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	if (!copy_request(req, &request, sizeof(request)) || request.signal_cap == CAP_ID_INVALID)
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	syscall_result_t result = kernel_signal_retain_cap(request.signal_cap, req->caller, CAP_SIGNAL, &signal);
	if (result.status != SYSCALL_STATUS_OK) return result;
	enum interrupt_result interrupt_result = interrupt_bind(managed->interrupt, signal);
	signal_release(signal);
	return interrupt_result_to_syscall(interrupt_result);
}

static syscall_result_t kernel_interrupt_unbind_handler(const struct cap_request* req,
                                                        struct kernel_interrupt*  managed) {
	struct interrupt_simple_request request;
	if ((req->rights & CAP_MANAGE) == 0u) return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	if (!copy_request(req, &request, sizeof(request))) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	return interrupt_result_to_syscall(interrupt_unbind(managed->interrupt));
}

static syscall_result_t kernel_interrupt_destroy_handler(const struct cap_request* req,
                                                         struct kernel_interrupt*  managed) {
	struct interrupt_simple_request request;
	if ((req->rights & CAP_DESTROY) == 0u) return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	if (!copy_request(req, &request, sizeof(request))) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	enum interrupt_result result = kernel_interrupt_destroy(managed);
	if (result != INTERRUPT_OK) return interrupt_result_to_syscall(result);
	(void)cap_object_destroy_with_id(managed->cap_object_id);
	return syscall_result_ok(0u);
}

static syscall_result_t kernel_interrupt_handler(const struct cap_request* req) {
	struct interrupt_request_header header;
	struct kernel_interrupt*        managed;
	if (req == NULL || req->object_id == 0u || req->request == NULL || req->request_size < sizeof(header))
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	memcpy(&header, req->request, sizeof(header));
	managed = (struct kernel_interrupt*)(uintptr_t)req->object_id;
	switch (header.op) {
	case INTERRUPT_OP_INFO:
		return kernel_interrupt_info_handler(req, managed);
	case INTERRUPT_OP_BIND:
		return kernel_interrupt_bind_handler(req, managed);
	case INTERRUPT_OP_UNBIND:
		return kernel_interrupt_unbind_handler(req, managed);
	case INTERRUPT_OP_DESTROY:
		return kernel_interrupt_destroy_handler(req, managed);
	default:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
}

static syscall_result_t interrupts_claim_source_handler(const struct cap_request* req) {
	struct interrupts_claim_source_request  request;
	struct interrupts_claim_source_response response = {0};
	struct interrupt*                       interrupt;
	if ((req->rights & CAP_MANAGE) == 0u) return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	if (!copy_request(req, &request, sizeof(request)) || !cap_kernel_response_fits(req, sizeof(response)))
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	enum interrupt_result interrupt_result =
		interrupt_claim_source(request.source, request.trigger, request.polarity, &interrupt);
	if (interrupt_result != INTERRUPT_OK) return interrupt_result_to_syscall(interrupt_result);
	response.interrupt_cap = kernel_interrupt_publish(interrupt, req->caller);
	if (response.interrupt_cap == CAP_ID_INVALID) {
		interrupt_release(interrupt);
		return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	}
	syscall_result_t result = write_published_response(req, &response, sizeof(response), response.interrupt_cap);
	if (result.status != SYSCALL_STATUS_OK) (void)cap_destroy_by_id(response.interrupt_cap);
	interrupt_release(interrupt);
	return result;
}

static syscall_result_t interrupts_resolve_source_handler(const struct cap_request* req) {
	struct interrupts_resolve_source_request  request;
	struct interrupts_resolve_source_response response = {0};
	if ((req->rights & CAP_READ) == 0u) return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	if (!copy_request(req, &request, sizeof(request)) || !cap_kernel_response_fits(req, sizeof(response)))
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	enum interrupt_result result =
		interrupt_resolve_source(request.controller_register_address, request.local_source_id, &response.source);
	if (result != INTERRUPT_OK) return interrupt_result_to_syscall(result);
	return cap_kernel_write_response(req, &response, sizeof(response));
}

static syscall_result_t interrupts_resolve_message_context_handler(const struct cap_request* req) {
	struct interrupts_resolve_message_context_request  request;
	struct interrupts_resolve_message_context_response response = {0};
	if ((req->rights & CAP_READ) == 0u) return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	if (!copy_request(req, &request, sizeof(request)) || request.reserved != 0u ||
	    !cap_kernel_response_fits(req, sizeof(response)))
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	enum interrupt_result result =
		interrupt_resolve_message_context(request.controller_register_address, request.producer_id, &response.context);
	if (result != INTERRUPT_OK) return interrupt_result_to_syscall(result);
	return cap_kernel_write_response(req, &response, sizeof(response));
}

static syscall_result_t interrupts_allocate_message_handler(const struct cap_request* req) {
	struct interrupts_allocate_message_request  request;
	struct interrupts_allocate_message_response response = {0};
	struct interrupt_message                    message  = {0};
	struct interrupt*                           interrupt;
	if ((req->rights & CAP_ALLOCATE) == 0u) return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	if (!copy_request(req, &request, sizeof(request)) || !cap_kernel_response_fits(req, sizeof(response)))
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	enum interrupt_result interrupt_result = interrupt_allocate_message(request.context, &interrupt, &message);
	if (interrupt_result != INTERRUPT_OK) return interrupt_result_to_syscall(interrupt_result);
	response.interrupt_cap = kernel_interrupt_publish(interrupt, req->caller);
	if (response.interrupt_cap == CAP_ID_INVALID) {
		interrupt_release(interrupt);
		return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	}
	response.message_address = message.message_address;
	response.message_data    = message.message_data;
	syscall_result_t result  = write_published_response(req, &response, sizeof(response), response.interrupt_cap);
	if (result.status != SYSCALL_STATUS_OK) (void)cap_destroy_by_id(response.interrupt_cap);
	interrupt_release(interrupt);
	return result;
}

static syscall_result_t interrupts_resource_handler(const struct cap_request* req) {
	struct interrupts_request_header header;
	if (req == NULL || req->request == NULL || req->request_size < sizeof(header))
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	memcpy(&header, req->request, sizeof(header));
	switch (header.op) {
	case INTERRUPTS_OP_RESOLVE_SOURCE:
		return interrupts_resolve_source_handler(req);
	case INTERRUPTS_OP_RESOLVE_MESSAGE_CONTEXT:
		return interrupts_resolve_message_context_handler(req);
	case INTERRUPTS_OP_CLAIM_SOURCE:
		return interrupts_claim_source_handler(req);
	case INTERRUPTS_OP_ALLOCATE_MESSAGE:
		return interrupts_allocate_message_handler(req);
	default:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
}

bool kernel_capability_interrupts_init(void) {
	if (interrupts_resource_object_id != CAP_OBJECT_ID_INVALID) return true;
	interrupts_resource_object_id = cap_object_create_kernel(0u, interrupts_resource_handler, NULL);
	return interrupts_resource_object_id != CAP_OBJECT_ID_INVALID;
}

cap_id_t kernel_capability_interrupts_grant(process_id_t recipient) {
	if (!kernel_capability_interrupts_available() || recipient == PROCESS_PID_INVALID) return CAP_ID_INVALID;
	return cap_create(interrupts_resource_object_id, recipient, INTERRUPTS_RESOURCE_RIGHTS, NULL);
}

bool kernel_capability_interrupts_available(void) {
	return interrupts_resource_object_id != CAP_OBJECT_ID_INVALID;
}
