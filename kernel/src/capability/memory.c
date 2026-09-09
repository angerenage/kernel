#include "memory.h"

#include <base/memory.h>
#include <core/address_space.h>
#include <core/capability.h>
#include <core/process.h>
#include <core/spinlock.h>
#include <core/syscall.h>
#include <kernel/capability.h>
#include <stdlib.h>
#include <string.h>

struct mapping_state {
	process_id_t          space_owner;
	struct mapping*       mapping;
	memory_access_t       maximum_access;
	struct mapping_state* next;
};

#define MEMORY_CAP_RIGHTS                                                                                              \
	((cap_rights_t)(CAP_CALL | CAP_READ | CAP_WRITE | CAP_EXEC | CAP_MAP | CAP_DERIVE | CAP_DELEGATE))
#define MAPPING_CAP_RIGHTS                                                                                             \
	((cap_rights_t)(CAP_CALL | CAP_READ | CAP_WRITE | CAP_EXEC | CAP_MAP | CAP_DESTROY | CAP_DELEGATE |                \
	                CAP_DELEGATE_PEER))
#define MAPPING_RESOURCE_BUCKET_COUNT 64u

static struct spinlock mapping_resource_lock =
	SPINLOCK_INIT_CLASS("mapping_resources", SPINLOCK_ORDER_CAPABILITY, SPINLOCK_FLAG_IRQSAVE);
static struct mapping_state* mapping_resources[MAPPING_RESOURCE_BUCKET_COUNT];

static syscall_result_t memory_handler(const struct cap_request* req);
static syscall_result_t mapping_handler(const struct cap_request* req);

static syscall_result_t copy_request(const struct cap_request* req, void* out, size_t size) {
	if (req == NULL || req->request == NULL || out == NULL || req->request_size < size)
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	memcpy(out, req->request, size);
	return syscall_result_ok(0u);
}

static bool access_valid(memory_access_t access) {
	return (access & ~MEMORY_ACCESS_VALID_MASK) == 0u;
}

static cap_rights_t access_rights(memory_access_t access) {
	cap_rights_t rights = 0u;
	if ((access & MEMORY_ACCESS_READ) != 0u) rights |= CAP_READ;
	if ((access & MEMORY_ACCESS_WRITE) != 0u) rights |= CAP_WRITE;
	if ((access & MEMORY_ACCESS_EXEC) != 0u) rights |= CAP_EXEC;
	return rights;
}

static void memory_destroy(uint64_t object_id) {
	memory_release((struct memory*)(uintptr_t)object_id);
}

static void destroy_unused_object(struct cap_object* object, enum cap_object_event event) {
	if (event == CAP_OBJECT_EVENT_ZERO_GRANTS) (void)cap_object_destroy_if_unused(object);
}

static cap_id_t memory_publish(struct memory* memory, process_id_t recipient, cap_rights_t rights,
                               struct capability* parent) {
	cap_object_id_t object_id;
	cap_id_t        cap;
	bool            created = false;
	if (memory == NULL) return CAP_ID_INVALID;
	if (recipient == PROCESS_PID_INVALID || (rights & CAP_CALL) == 0u || (rights & ~MEMORY_CAP_RIGHTS) != 0u) {
		memory_release(memory);
		return CAP_ID_INVALID;
	}
	object_id = cap_object_create_kernel_lifecycle(
		(uint64_t)(uintptr_t)memory, memory_handler, NULL, memory_destroy, destroy_unused_object, &created);
	if (object_id == CAP_OBJECT_ID_INVALID) {
		memory_release(memory);
		return CAP_ID_INVALID;
	}
	if (!created) memory_release(memory);
	cap = cap_create(object_id, recipient, rights, parent);
	if (cap == CAP_ID_INVALID && created) (void)cap_object_destroy_with_id(object_id);
	return cap;
}

cap_id_t kernel_memory_grant(struct memory* memory, process_id_t recipient, cap_rights_t rights) {
	if (!memory_retain(memory)) return CAP_ID_INVALID;
	return memory_publish(memory, recipient, rights, NULL);
}

syscall_result_t kernel_memory_acquire(cap_id_t memory_cap, process_id_t caller, cap_rights_t required_rights,
                                       struct cap_object** out_object, struct memory** out_memory,
                                       cap_rights_t* out_rights) {
	struct cap_object* object;
	cap_rights_t       rights;
	if (out_object == NULL || out_memory == NULL || memory_cap == CAP_ID_INVALID || caller == PROCESS_PID_INVALID)
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	*out_object            = NULL;
	*out_memory            = NULL;
	enum cap_result result = cap_object_acquire_for_use(caller, memory_cap, required_rights, &object, &rights);
	if (result != CAP_OK)
		return syscall_result_error(result == CAP_NOT_AUTHORIZED || result == CAP_RIGHTS_EXCEEDED
		                                ? SYSCALL_STATUS_DENIED
		                                : SYSCALL_STATUS_BAD_ARGUMENT,
		                            0u);
	if (object == NULL || object->handler != memory_handler || object->object_id == 0u) {
		cap_object_release(object);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	*out_object = object;
	*out_memory = (struct memory*)(uintptr_t)object->object_id;
	if (out_rights != NULL) *out_rights = rights;
	return syscall_result_ok(0u);
}

static syscall_result_t memory_info_handler(const struct cap_request* req, struct memory* memory) {
	if ((req->rights & CAP_READ) == 0u) return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	const struct memory_info response = {.size = memory_size(memory), .memory_type = memory_type(memory)};
	return cap_kernel_write_response(req, &response, sizeof(response));
}

static syscall_result_t memory_slice_handler(const struct cap_request* req, struct memory* memory) {
	struct memory_slice_request  request;
	struct memory_slice_response response = {.memory_cap = CAP_ID_INVALID};
	struct memory*               slice;
	struct capability*           parent;
	syscall_result_t             result;
	if ((req->rights & CAP_DERIVE) == 0u) return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	result = copy_request(req, &request, sizeof(request));
	if (result.status != SYSCALL_STATUS_OK || !cap_kernel_response_fits(req, sizeof(response)))
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	if (!memory_slice(memory, request.offset, request.size, &slice))
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	parent = cap_acquire(req->cap_id);
	if (parent == NULL) {
		memory_release(slice);
		return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	}
	response.memory_cap = memory_publish(slice, req->caller, req->rights, parent);
	cap_release(parent);
	if (response.memory_cap == CAP_ID_INVALID) {
		return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	}
	result = cap_kernel_write_response(req, &response, sizeof(response));
	if (result.status != SYSCALL_STATUS_OK) (void)cap_destroy_by_id(response.memory_cap);
	return result;
}

static syscall_result_t memory_handler(const struct cap_request* req) {
	struct memory_request_header header;
	if (req == NULL || req->object_id == 0u || copy_request(req, &header, sizeof(header)).status != SYSCALL_STATUS_OK)
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	struct memory* memory = (struct memory*)(uintptr_t)req->object_id;
	switch (header.op) {
	case MEMORY_OP_INFO:
		return memory_info_handler(req, memory);
	case MEMORY_OP_SLICE:
		return memory_slice_handler(req, memory);
	default:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
}

static bool mapping_unmap_state(const struct mapping_state* state) {
	struct process* owner;
	bool            result;
	if (state == NULL) return false;
	owner = process_acquire(state->space_owner);
	if (owner == NULL) return false;
	result = address_space_unmap(process_address_space(owner), state->mapping);
	process_release(owner);
	return result;
}

static bool mapping_resource_register(struct mapping_state* state) {
	size_t           bucket = ((uintptr_t)state->mapping >> 4u) % MAPPING_RESOURCE_BUCKET_COUNT;
	struct irq_state irq    = spinlock_lock_irqsave(&mapping_resource_lock);
	for (const struct mapping_state* current = mapping_resources[bucket]; current != NULL; current = current->next) {
		if (current->mapping == state->mapping) {
			spinlock_unlock_irqrestore(&mapping_resource_lock, irq);
			return false;
		}
	}
	state->next               = mapping_resources[bucket];
	mapping_resources[bucket] = state;
	spinlock_unlock_irqrestore(&mapping_resource_lock, irq);
	return true;
}

static void mapping_resource_unregister(struct mapping_state* state) {
	size_t                 bucket = ((uintptr_t)state->mapping >> 4u) % MAPPING_RESOURCE_BUCKET_COUNT;
	struct irq_state       irq    = spinlock_lock_irqsave(&mapping_resource_lock);
	struct mapping_state** link   = &mapping_resources[bucket];
	while (*link != NULL && *link != state) link = &(*link)->next;
	if (*link == state) *link = state->next;
	spinlock_unlock_irqrestore(&mapping_resource_lock, irq);
}

static void mapping_destroy(uint64_t object_id) {
	struct mapping_state* state = (struct mapping_state*)(uintptr_t)object_id;
	if (state == NULL) return;
	mapping_resource_unregister(state);
	(void)mapping_unmap_state(state);
	mapping_release(state->mapping);
	free(state);
}

static void mapping_event(struct cap_object* object, enum cap_object_event event) {
	if (event != CAP_OBJECT_EVENT_ZERO_GRANTS) return;
	(void)mapping_unmap_state((const struct mapping_state*)(uintptr_t)object->object_id);
	(void)cap_object_destroy_if_unused(object);
}

static bool mapping_process_cleanup(uint64_t object_id, process_id_t process) {
	const struct mapping_state* state = (const struct mapping_state*)(uintptr_t)object_id;
	return state != NULL && state->space_owner == process;
}

static cap_id_t mapping_publish_unique(process_id_t recipient, process_id_t owner, struct mapping* mapping,
                                       cap_rights_t rights, memory_access_t maximum_access) {
	struct mapping_state* state;
	cap_object_id_t       object_id;
	cap_id_t              cap;
	if (recipient == PROCESS_PID_INVALID || owner == PROCESS_PID_INVALID || mapping == NULL ||
	    (rights & CAP_CALL) == 0u || (rights & ~MAPPING_CAP_RIGHTS) != 0u || !access_valid(maximum_access))
		return CAP_ID_INVALID;
	state = malloc(sizeof(*state));
	if (state == NULL || !mapping_retain(mapping)) {
		free(state);
		return CAP_ID_INVALID;
	}
	*state = (struct mapping_state){
		.space_owner    = owner,
		.mapping        = mapping,
		.maximum_access = maximum_access,
	};
	if (!mapping_resource_register(state)) {
		mapping_release(mapping);
		free(state);
		return CAP_ID_INVALID;
	}
	object_id = cap_object_create_kernel_lifecycle(
		(uint64_t)(uintptr_t)state, mapping_handler, mapping_process_cleanup, mapping_destroy, mapping_event, NULL);
	if (object_id == CAP_OBJECT_ID_INVALID) {
		mapping_resource_unregister(state);
		mapping_release(mapping);
		free(state);
		return CAP_ID_INVALID;
	}
	cap = cap_create(object_id, recipient, rights, NULL);
	if (cap == CAP_ID_INVALID) (void)cap_object_destroy_with_id(object_id);
	return cap;
}

cap_id_t kernel_mapping_publish(struct process* target, process_id_t recipient, struct mapping* mapping,
                                cap_rights_t rights, memory_access_t maximum_access) {
	struct process* retained;
	if (target == NULL || recipient == PROCESS_PID_INVALID || mapping == NULL) return CAP_ID_INVALID;
	retained = process_acquire(process_pid(target));
	if (retained == NULL || retained != target ||
	    !address_space_contains_mapping(process_address_space(retained), mapping)) {
		process_release(retained);
		return CAP_ID_INVALID;
	}
	cap_id_t cap = mapping_publish_unique(recipient, process_pid(retained), mapping, rights, maximum_access);
	process_release(retained);
	return cap;
}

bool kernel_mapping_discard_unpublished(cap_id_t mapping_cap, process_id_t owner) {
	struct cap_object* object;
	if (mapping_cap == CAP_ID_INVALID || owner == PROCESS_PID_INVALID ||
	    cap_object_acquire_for_use(owner, mapping_cap, 0u, &object, NULL) != CAP_OK)
		return false;
	bool valid  = object != NULL && object->handler == mapping_handler;
	bool result = valid && cap_object_destroy(object);
	cap_object_release(object);
	return result;
}

static syscall_result_t mapping_info_handler(const struct cap_request* req, const struct mapping_state* state) {
	struct process* owner;
	if ((req->rights & CAP_READ) == 0u) return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	owner = process_acquire(state->space_owner);
	if (owner == NULL || !address_space_contains_mapping(process_address_space(owner), state->mapping)) {
		process_release(owner);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	const struct mapping_info response = {
		.address      = mapping_address(state->mapping),
		.size         = mapping_size(state->mapping),
		.access       = mapping_access(state->mapping),
		.guard_before = mapping_guard_before(state->mapping),
		.guard_after  = mapping_guard_after(state->mapping),
		.memory_type  = mapping_memory_type(state->mapping),
	};
	syscall_result_t result = cap_kernel_write_response(req, &response, sizeof(response));
	process_release(owner);
	return result;
}

static syscall_result_t mapping_protect_handler(const struct cap_request* req, const struct mapping_state* state) {
	struct mapping_protect_request request;
	struct process*                owner;
	if (copy_request(req, &request, sizeof(request)).status != SYSCALL_STATUS_OK || !access_valid(request.access))
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	cap_rights_t required = CAP_MAP | access_rights(request.access);
	if ((req->rights & required) != required || (request.access & ~state->maximum_access) != 0u)
		return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	owner = process_acquire(state->space_owner);
	if (owner == NULL) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	bool changed = address_space_protect(process_address_space(owner), state->mapping, request.access);
	process_release(owner);
	return changed ? syscall_result_ok(0u) : syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
}

static syscall_result_t mapping_unmap_handler(const struct cap_request* req, const struct mapping_state* state) {
	struct capability* cap = cap_acquire(req->cap_id);
	if (cap == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	cap_object_id_t object_id = cap->cap_object_id;
	if (!mapping_unmap_state(state)) {
		cap_release(cap);
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
	bool destroyed = cap_object_destroy_with_id(object_id);
	cap_release(cap);
	return destroyed ? syscall_result_ok(0u) : syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
}

static syscall_result_t mapping_handler(const struct cap_request* req) {
	struct mapping_request_header header;
	if (req == NULL || req->object_id == 0u || copy_request(req, &header, sizeof(header)).status != SYSCALL_STATUS_OK)
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	const struct mapping_state* state = (const struct mapping_state*)(uintptr_t)req->object_id;
	switch (header.op) {
	case MAPPING_OP_INFO:
		return mapping_info_handler(req, state);
	case MAPPING_OP_PROTECT:
		return mapping_protect_handler(req, state);
	case MAPPING_OP_UNMAP:
		if ((req->rights & CAP_DESTROY) == 0u) return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
		return mapping_unmap_handler(req, state);
	default:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
}
