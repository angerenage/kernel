#include "dma.h"

#include <base/dma.h>
#include <base/syscall.h>
#include <core/address_space.h>
#include <core/capability.h>
#include <core/dma.h>
#include <core/spinlock.h>
#include <hal/hcf.h>
#include <kernel/capability.h>
#include <libc/stdlib.h>
#include <string.h>

#include "address_space.h"

#define DMA_RESOURCE_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_READ | CAP_MANAGE | CAP_DELEGATE))
#define DMA_SOURCE_CAP_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_ALLOCATE | CAP_MANAGE | CAP_DELEGATE))
#define DMA_ADDRESS_SPACE_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_READ | CAP_WRITE | CAP_MAP | CAP_DELEGATE))
#define DMA_BINDING_CAP_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_DESTROY | CAP_DELEGATE))

struct kernel_dma_source {
	dma_source_t              source;
	struct kernel_dma_source* next;
};

static cap_object_id_t           dma_object_id = CAP_OBJECT_ID_INVALID;
static struct kernel_dma_source* claimed_sources;
static struct spinlock           claimed_sources_lock =
	SPINLOCK_INIT_CLASS("dma_source_claims", SPINLOCK_ORDER_CAPABILITY, SPINLOCK_FLAG_IRQSAVE);

#if defined(KERNEL_CAPABILITY_DMA_TEST)
static bool     fail_next_claim_response;
static cap_id_t last_rollback_cap = CAP_ID_INVALID;

void kernel_capability_dma_test_fail_next_claim_response(void) {
	fail_next_claim_response = true;
	last_rollback_cap        = CAP_ID_INVALID;
}

cap_id_t kernel_capability_dma_test_last_rollback_cap(void) {
	return last_rollback_cap;
}
#endif

static syscall_result_t dma_handler(const struct cap_request* req);
static syscall_result_t dma_source_handler(const struct cap_request* req);
static syscall_result_t dma_binding_handler(const struct cap_request* req);

static bool copy_request(const struct cap_request* req, void* out, size_t size) {
	if (req == NULL || req->request == NULL || out == NULL || req->request_size != size) return false;
	memcpy(out, req->request, size);
	return true;
}

static syscall_result_t write_claim_response(const struct cap_request*               req,
                                             const struct dma_claim_source_response* response) {
#if defined(KERNEL_CAPABILITY_DMA_TEST)
	if (fail_next_claim_response) {
		fail_next_claim_response = false;
		last_rollback_cap        = response->source_cap;
		return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	}
#endif
	return cap_kernel_write_response(req, response, sizeof(*response));
}

static void dma_binding_destroy(uint64_t object_id) {
	dma_binding_release((struct dma_binding*)(uintptr_t)object_id);
}

static void dma_binding_event(struct cap_object* object, enum cap_object_event event) {
	if (event == CAP_OBJECT_EVENT_ZERO_GRANTS) (void)cap_object_destroy_if_unused(object);
}

static cap_id_t dma_binding_publish(struct dma_binding* binding, process_id_t recipient) {
	cap_object_id_t object_id;
	cap_id_t        cap;
	bool            created = false;

	if (binding == NULL || recipient == PROCESS_PID_INVALID || !dma_binding_retain(binding)) return CAP_ID_INVALID;
	object_id = cap_object_create_kernel_lifecycle(
		(uint64_t)(uintptr_t)binding, dma_binding_handler, NULL, dma_binding_destroy, dma_binding_event, &created);
	if (object_id == CAP_OBJECT_ID_INVALID) {
		dma_binding_release(binding);
		return CAP_ID_INVALID;
	}
	if (!created) dma_binding_release(binding);

	cap = cap_create(object_id, recipient, DMA_BINDING_CAP_RIGHTS, NULL);
	if (cap == CAP_ID_INVALID && created) (void)cap_object_destroy_with_id(object_id);
	return cap;
}

static void dma_source_remove(struct kernel_dma_source* managed) {
	struct irq_state           state;
	struct kernel_dma_source** link;

	if (managed == NULL) return;
	state = spinlock_lock_irqsave(&claimed_sources_lock);
	for (link = &claimed_sources; *link != NULL && *link != managed; link = &(*link)->next) {
	}
	if (*link == managed) *link = managed->next;
	spinlock_unlock_irqrestore(&claimed_sources_lock, state);
}

static void dma_source_destroy(uint64_t object_id) {
	struct kernel_dma_source* managed = (struct kernel_dma_source*)(uintptr_t)object_id;
	struct dma_binding*       binding;

	if (managed == NULL) return;
	if (dma_binding_recover(managed->source, &binding)) {
		if (!dma_unbind(binding) && dma_binding_is_active(binding)) hcf();
		dma_binding_release(binding);
	}
	dma_source_remove(managed);
	free(managed);
}

static void dma_source_event(struct cap_object* object, enum cap_object_event event) {
	if (event == CAP_OBJECT_EVENT_ZERO_GRANTS) (void)cap_object_destroy_if_unused(object);
}

static cap_id_t dma_source_claim(dma_source_t source, process_id_t recipient, bool* out_unavailable) {
	struct kernel_dma_source* managed;
	struct irq_state          state;
	cap_object_id_t           object_id;
	cap_id_t                  cap;
	bool                      created = false;

	if (out_unavailable != NULL) *out_unavailable = false;
	if (recipient == PROCESS_PID_INVALID || out_unavailable == NULL || !dma_source_valid(source)) return CAP_ID_INVALID;
	managed = calloc(1u, sizeof(*managed));
	if (managed == NULL) return CAP_ID_INVALID;
	managed->source = source;

	state = spinlock_lock_irqsave(&claimed_sources_lock);
	for (const struct kernel_dma_source* current = claimed_sources; current != NULL; current = current->next) {
		if (current->source == source) {
			spinlock_unlock_irqrestore(&claimed_sources_lock, state);
			free(managed);
			*out_unavailable = true;
			return CAP_ID_INVALID;
		}
	}
	managed->next   = claimed_sources;
	claimed_sources = managed;
	spinlock_unlock_irqrestore(&claimed_sources_lock, state);

	object_id = cap_object_create_kernel_lifecycle(
		(uint64_t)(uintptr_t)managed, dma_source_handler, NULL, dma_source_destroy, dma_source_event, &created);
	if (object_id == CAP_OBJECT_ID_INVALID || !created) {
		dma_source_remove(managed);
		free(managed);
		return CAP_ID_INVALID;
	}
	cap = cap_create(object_id, recipient, DMA_SOURCE_CAP_RIGHTS, NULL);
	if (cap == CAP_ID_INVALID) (void)cap_object_destroy_with_id(object_id);
	return cap;
}

static syscall_result_t dma_resolve_source_handler(const struct cap_request* req) {
	struct dma_resolve_source_request  request;
	struct dma_resolve_source_response response = {.source = DMA_SOURCE_INVALID};

	if ((req->rights & CAP_READ) == 0u) return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	if (!copy_request(req, &request, sizeof(request)) || request.reserved != 0u ||
	    !cap_kernel_response_fits(req, sizeof(response)))
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	if (!dma_source_resolve(request.controller_register_address, request.local_source_id, &response.source))
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	return cap_kernel_write_response(req, &response, sizeof(response));
}

static syscall_result_t dma_claim_source_handler(const struct cap_request* req) {
	struct dma_claim_source_request  request;
	struct dma_claim_source_response response = {.source_cap = CAP_ID_INVALID};
	bool                             unavailable;

	if ((req->rights & CAP_MANAGE) == 0u) return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	if (!copy_request(req, &request, sizeof(request)) || request.source == DMA_SOURCE_INVALID ||
	    !cap_kernel_response_fits(req, sizeof(response)))
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	if (!dma_source_valid(request.source)) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	response.source_cap = dma_source_claim(request.source, req->caller, &unavailable);
	if (response.source_cap == CAP_ID_INVALID)
		return syscall_result_error(unavailable ? SYSCALL_STATUS_UNAVAILABLE : SYSCALL_STATUS_FAILED, 0u);
	syscall_result_t result = write_claim_response(req, &response);
	if (result.status != SYSCALL_STATUS_OK) (void)cap_destroy_by_id(response.source_cap);
	return result;
}

static syscall_result_t dma_source_create_address_space_handler(const struct cap_request* req, dma_source_t source) {
	struct dma_source_create_address_space_request  request;
	struct dma_source_create_address_space_response response = {.address_space_cap = CAP_ID_INVALID};
	struct address_space*                           space;

	if ((req->rights & CAP_ALLOCATE) == 0u) return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	if (!copy_request(req, &request, sizeof(request)) || !cap_kernel_response_fits(req, sizeof(response)))
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	if (!dma_address_space_create(source, &space)) return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);

	response.address_space_cap = kernel_device_address_space_grant(space, req->caller, DMA_ADDRESS_SPACE_RIGHTS);
	address_space_device_release(space);
	if (response.address_space_cap == CAP_ID_INVALID) return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);

	syscall_result_t result = cap_kernel_write_response(req, &response, sizeof(response));
	if (result.status != SYSCALL_STATUS_OK) (void)cap_destroy_by_id(response.address_space_cap);
	return result;
}

static syscall_result_t dma_source_bind_handler(const struct cap_request* req, dma_source_t source) {
	struct dma_source_bind_request  request;
	struct dma_source_bind_response response = {.binding_cap = CAP_ID_INVALID};
	struct cap_object*              space_object;
	struct address_space*           space;
	struct dma_binding*             binding;

	if ((req->rights & CAP_MANAGE) == 0u) return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	if (!copy_request(req, &request, sizeof(request)) || !cap_kernel_response_fits(req, sizeof(response)))
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);

	syscall_result_t result = kernel_device_address_space_acquire(
		request.address_space_cap, req->caller, CAP_MAP, &space_object, &space, NULL);
	if (result.status != SYSCALL_STATUS_OK) return result;
	if (!dma_bind(source, space, &binding)) {
		cap_object_release(space_object);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	cap_object_release(space_object);

	response.binding_cap = dma_binding_publish(binding, req->caller);
	if (response.binding_cap == CAP_ID_INVALID) {
		if (!dma_unbind(binding) && dma_binding_is_active(binding)) hcf();
		dma_binding_release(binding);
		return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	}

	result = cap_kernel_write_response(req, &response, sizeof(response));
	if (result.status != SYSCALL_STATUS_OK) {
		if (!dma_unbind(binding) && dma_binding_is_active(binding)) hcf();
		(void)cap_destroy_by_id(response.binding_cap);
	}
	dma_binding_release(binding);
	return result;
}

static syscall_result_t dma_source_recover_handler(const struct cap_request* req, dma_source_t source) {
	struct dma_source_recover_request  request;
	struct dma_source_recover_response response = {.binding_cap = CAP_ID_INVALID};
	struct dma_binding*                binding;

	if ((req->rights & CAP_MANAGE) == 0u) return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	if (!copy_request(req, &request, sizeof(request)) || !cap_kernel_response_fits(req, sizeof(response)))
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	if (!dma_binding_recover(source, &binding)) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);

	response.binding_cap = dma_binding_publish(binding, req->caller);
	dma_binding_release(binding);
	if (response.binding_cap == CAP_ID_INVALID) return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);

	syscall_result_t result = cap_kernel_write_response(req, &response, sizeof(response));
	if (result.status != SYSCALL_STATUS_OK) (void)cap_destroy_by_id(response.binding_cap);
	return result;
}

static syscall_result_t dma_handler(const struct cap_request* req) {
	struct dma_request_header header;
	if (req == NULL || req->request == NULL || req->request_size < sizeof(header))
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	memcpy(&header, req->request, sizeof(header));

	switch (header.op) {
	case DMA_OP_RESOLVE_SOURCE:
		return dma_resolve_source_handler(req);
	case DMA_OP_CLAIM_SOURCE:
		return dma_claim_source_handler(req);
	default:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
}

static syscall_result_t dma_source_handler(const struct cap_request* req) {
	struct dma_source_request_header header;
	struct kernel_dma_source*        managed;

	if (req == NULL || req->object_id == 0u || req->request == NULL || req->request_size < sizeof(header))
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	memcpy(&header, req->request, sizeof(header));
	managed = (struct kernel_dma_source*)(uintptr_t)req->object_id;

	switch (header.op) {
	case DMA_SOURCE_OP_CREATE_ADDRESS_SPACE:
		return dma_source_create_address_space_handler(req, managed->source);
	case DMA_SOURCE_OP_BIND:
		return dma_source_bind_handler(req, managed->source);
	case DMA_SOURCE_OP_RECOVER:
		return dma_source_recover_handler(req, managed->source);
	default:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
}

static syscall_result_t dma_binding_unbind_handler(const struct cap_request* req, struct dma_binding* binding) {
	struct dma_binding_unbind_request request;
	struct capability*                cap;

	if ((req->rights & CAP_DESTROY) == 0u) return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	if (!copy_request(req, &request, sizeof(request))) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);

	cap = cap_acquire(req->cap_id);
	if (cap == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	cap_object_id_t object_id = cap->cap_object_id;
	if (!dma_unbind(binding)) {
		cap_release(cap);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	bool destroyed = cap_object_destroy_with_id(object_id);
	cap_release(cap);
	return destroyed ? syscall_result_ok(0u) : syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
}

static syscall_result_t dma_binding_handler(const struct cap_request* req) {
	struct dma_binding_request_header header;
	if (req == NULL || req->object_id == 0u || req->request == NULL || req->request_size < sizeof(header))
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	memcpy(&header, req->request, sizeof(header));

	struct dma_binding* binding = (struct dma_binding*)(uintptr_t)req->object_id;
	switch (header.op) {
	case DMA_BINDING_OP_UNBIND:
		return dma_binding_unbind_handler(req, binding);
	default:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
}

bool kernel_capability_dma_init(void) {
	if (!dma_available()) return true;
	if (dma_object_id != CAP_OBJECT_ID_INVALID) return true;
	dma_object_id = cap_object_create_kernel(0u, dma_handler, NULL);
	return dma_object_id != CAP_OBJECT_ID_INVALID;
}

bool kernel_capability_dma_available(void) {
	return dma_available() && dma_object_id != CAP_OBJECT_ID_INVALID;
}

cap_id_t kernel_capability_dma_grant(process_id_t recipient) {
	if (!kernel_capability_dma_available() || recipient == PROCESS_PID_INVALID) return CAP_ID_INVALID;
	return cap_create(dma_object_id, recipient, DMA_RESOURCE_RIGHTS, NULL);
}
