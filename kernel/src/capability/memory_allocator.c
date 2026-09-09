#include "memory_allocator.h"

#include <base/math.h>
#include <base/memory.h>
#include <core/capability.h>
#include <core/memory.h>
#include <core/pmm.h>
#include <core/syscall.h>
#include <hal/hcf.h>
#include <kernel/capability.h>
#include <stdlib.h>
#include <string.h>

#include "memory.h"

struct kernel_memory_allocator {
	struct kernel_memory_allocator*         parent;
	uint64_t                                reference_count;
	cap_rights_t                            memory_rights;
	uint64_t                                memory_type_mask;
	bool                                    unrestricted_physical_claims;
	struct memory_allocator_physical_range* claim_ranges;
	size_t                                  claim_range_count;
};

static struct kernel_memory_allocator* root_allocator;
static syscall_result_t                allocator_handler(const struct cap_request* req);

#define ALLOCATOR_MEMORY_RIGHTS ((cap_rights_t)(CAP_READ | CAP_WRITE | CAP_EXEC | CAP_MAP | CAP_DERIVE | CAP_DELEGATE))
#define ALLOCATOR_CAP_RIGHTS                                                                                           \
	((cap_rights_t)(CAP_CALL | CAP_READ | CAP_ALLOCATE | CAP_DERIVE | CAP_MANAGE | CAP_DELEGATE | CAP_DELEGATE_PEER))
#define MEMORY_TYPE_VALID_MASK ((1ull << MEMORY_TYPE_COUNT) - 1u)

static bool allocator_retain(struct kernel_memory_allocator* allocator) {
	uint64_t current;
	if (allocator == NULL) return false;
	current = __atomic_load_n(&allocator->reference_count, __ATOMIC_ACQUIRE);
	for (;;) {
		if (current == 0u || current == UINT64_MAX) return false;
		if (__atomic_compare_exchange_n(
				&allocator->reference_count, &current, current + 1u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
			return true;
	}
}

static void allocator_release(struct kernel_memory_allocator* allocator) {
	while (allocator != NULL) {
		uint64_t old = __atomic_fetch_sub(&allocator->reference_count, 1u, __ATOMIC_ACQ_REL);
		if (old == 0u) hcf();
		if (old != 1u) return;
		struct kernel_memory_allocator* parent = allocator->parent;
		free(allocator->claim_ranges);
		free(allocator);
		allocator = parent;
	}
}

static void allocator_destroy(uint64_t object_id) {
	allocator_release((struct kernel_memory_allocator*)(uintptr_t)object_id);
}

static void allocator_event(struct cap_object* object, enum cap_object_event event) {
	if (event == CAP_OBJECT_EVENT_ZERO_GRANTS) (void)cap_object_destroy_if_unused(object);
}

static cap_id_t allocator_publish(struct kernel_memory_allocator* allocator, process_id_t recipient,
                                  cap_rights_t rights, struct capability* parent) {
	cap_object_id_t object_id;
	cap_id_t        cap;
	bool            created = false;
	if (allocator == NULL) return CAP_ID_INVALID;
	if (recipient == PROCESS_PID_INVALID || (rights & ~ALLOCATOR_CAP_RIGHTS) != 0u || (rights & CAP_CALL) == 0u) {
		allocator_release(allocator);
		return CAP_ID_INVALID;
	}
	object_id = cap_object_create_kernel_lifecycle(
		(uint64_t)(uintptr_t)allocator, allocator_handler, NULL, allocator_destroy, allocator_event, &created);
	if (object_id == CAP_OBJECT_ID_INVALID) {
		allocator_release(allocator);
		return CAP_ID_INVALID;
	}
	if (!created) allocator_release(allocator);
	cap = cap_create(object_id, recipient, rights, parent);
	if (cap == CAP_ID_INVALID && created) (void)cap_object_destroy_with_id(object_id);
	return cap;
}

static bool range_end(struct memory_allocator_physical_range range, uintptr_t* out_end) {
	return range.size != 0u && range.size <= UINTPTR_MAX - range.address &&
	       ((*out_end = range.address + range.size), true);
}

static bool range_allowed(const struct kernel_memory_allocator*  allocator,
                          struct memory_allocator_physical_range range) {
	uintptr_t end;
	if (!range_end(range, &end)) return false;
	if (allocator->unrestricted_physical_claims) return true;
	for (size_t i = 0u; i < allocator->claim_range_count; i++) {
		uintptr_t allowed_end = allocator->claim_ranges[i].address + allocator->claim_ranges[i].size;
		if (range.address >= allocator->claim_ranges[i].address && end <= allowed_end) return true;
	}
	return false;
}

static bool normalize_ranges(const struct memory_allocator_physical_range* input, size_t count,
                             struct memory_allocator_physical_range** out_ranges, size_t* out_count) {
	struct memory_allocator_physical_range* ranges;
	*out_ranges = NULL;
	*out_count  = 0u;
	if (count == 0u) return true;
	if (input == NULL || count > SIZE_MAX / sizeof(*ranges)) return false;
	ranges = malloc(count * sizeof(*ranges));
	if (ranges == NULL) return false;
	memcpy(ranges, input, count * sizeof(*ranges));
	for (size_t i = 0u; i < count; i++) {
		uintptr_t ignored;
		if (!range_end(ranges[i], &ignored)) {
			free(ranges);
			return false;
		}
		for (size_t j = i; j != 0u && ranges[j].address < ranges[j - 1u].address; j--) {
			struct memory_allocator_physical_range swap = ranges[j];
			ranges[j]                                   = ranges[j - 1u];
			ranges[j - 1u]                              = swap;
		}
	}
	size_t normalized = 0u;
	for (size_t i = 0u; i < count; i++) {
		if (normalized == 0u) {
			ranges[normalized++] = ranges[i];
			continue;
		}
		struct memory_allocator_physical_range* previous     = &ranges[normalized - 1u];
		uintptr_t                               previous_end = previous->address + previous->size;
		uintptr_t                               current_end  = ranges[i].address + ranges[i].size;
		if (ranges[i].address <= previous_end) {
			if (current_end > previous_end) previous->size = current_end - previous->address;
		}
		else ranges[normalized++] = ranges[i];
	}
	*out_ranges = ranges;
	*out_count  = normalized;
	return true;
}

static syscall_result_t allocator_info(const struct cap_request* req, const struct kernel_memory_allocator* allocator) {
	const struct pmm_info* pmm = pmm_info();
	if ((req->rights & CAP_READ) == 0u) return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	const struct memory_allocator_info response = {
		.memory_rights          = allocator->memory_rights,
		.memory_type_mask       = allocator->memory_type_mask,
		.claim_policy           = allocator->unrestricted_physical_claims ? MEMORY_ALLOCATOR_CLAIMS_UNRESTRICTED
	                              : allocator->claim_range_count == 0u    ? MEMORY_ALLOCATOR_CLAIMS_NONE
	                                                                      : MEMORY_ALLOCATOR_CLAIMS_RESTRICTED,
		.claim_range_count      = allocator->claim_range_count,
		.physical_claim_granule = pmm == NULL ? 0u : pmm->allocation_granule,
	};
	return cap_kernel_write_response(req, &response, sizeof(response));
}

static syscall_result_t allocator_alloc(const struct cap_request*             req,
                                        const struct kernel_memory_allocator* allocator) {
	struct memory_allocator_alloc_request  request;
	struct memory_allocator_alloc_response response = {.memory_cap = CAP_ID_INVALID};
	struct memory*                         memory;
	if ((req->rights & CAP_ALLOCATE) == 0u) return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	if (req->request_size < sizeof(request) || !cap_kernel_response_fits(req, sizeof(response)))
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	memcpy(&request, req->request, sizeof(request));
	if (request.size == 0u) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	if ((allocator->memory_type_mask & (1ull << MEMORY_TYPE_NORMAL)) == 0u)
		return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	if (!memory_create_anonymous(request.size, &memory)) return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	response.memory_cap =
		kernel_memory_grant(memory, req->caller, CAP_CALL | (allocator->memory_rights & ALLOCATOR_MEMORY_RIGHTS));
	memory_release(memory);
	if (response.memory_cap == CAP_ID_INVALID) return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	syscall_result_t result = cap_kernel_write_response(req, &response, sizeof(response));
	if (result.status != SYSCALL_STATUS_OK) (void)cap_destroy_by_id(response.memory_cap);
	return result;
}

static syscall_result_t allocator_claim(const struct cap_request*             req,
                                        const struct kernel_memory_allocator* allocator) {
	struct memory_allocator_claim_physical_request  request;
	struct memory_allocator_claim_physical_response response = {.memory_cap = CAP_ID_INVALID};
	struct memory*                                  memory;
	const struct pmm_info*                          pmm = pmm_info();
	if ((req->rights & CAP_MANAGE) == 0u) return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	if (req->request_size < sizeof(request) || !cap_kernel_response_fits(req, sizeof(response)) || pmm == NULL)
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	memcpy(&request, req->request, sizeof(request));
	uint64_t type_bit = (unsigned)request.memory_type < MEMORY_TYPE_COUNT ? 1ull << (unsigned)request.memory_type : 0u;
	if (type_bit == 0u || (allocator->memory_type_mask & type_bit) == 0u || request.size == 0u ||
	    ((request.physical_address | request.size) & (pmm->allocation_granule - 1u)) != 0u ||
	    !range_allowed(allocator, (struct memory_allocator_physical_range){request.physical_address, request.size}))
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	if (!memory_create_physical(
			&(const struct memory_physical_request){
				.physical_address        = request.physical_address,
				.size                    = request.size,
				.memory_type             = request.memory_type,
				.external_cpu_accessible = false,
			},
			&memory))
		return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	cap_rights_t memory_rights = allocator->memory_rights & ALLOCATOR_MEMORY_RIGHTS;
	if (request.memory_type == MEMORY_TYPE_DEVICE || !memory_cpu_accessible(memory)) memory_rights &= ~CAP_EXEC;
	response.memory_cap = kernel_memory_grant(memory, req->caller, CAP_CALL | memory_rights);
	memory_release(memory);
	if (response.memory_cap == CAP_ID_INVALID) return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	syscall_result_t result = cap_kernel_write_response(req, &response, sizeof(response));
	if (result.status != SYSCALL_STATUS_OK) (void)cap_destroy_by_id(response.memory_cap);
	return result;
}

static syscall_result_t allocator_derive(const struct cap_request* req, struct kernel_memory_allocator* parent) {
	struct memory_allocator_derive_request  header;
	struct memory_allocator_derive_response response = {.allocator_cap = CAP_ID_INVALID};
	struct kernel_memory_allocator*         child;
	struct capability*                      parent_cap;
	size_t                                  range_bytes;
	if ((req->rights & CAP_DERIVE) == 0u) return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	if (req->request_size < sizeof(header) || !cap_kernel_response_fits(req, sizeof(response)))
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	memcpy(&header, req->request, sizeof(header));
	if (mul_overflow_size((size_t)header.claim_range_count, sizeof(header.claim_ranges[0]), &range_bytes) ||
	    range_bytes != req->request_size - sizeof(header) ||
	    (header.flags & ~MEMORY_ALLOCATOR_DERIVE_INHERIT_CLAIMS) != 0u ||
	    (header.allocator_rights & ~req->rights) != 0u || (header.allocator_rights & CAP_CALL) == 0u ||
	    (header.memory_rights & ~parent->memory_rights) != 0u ||
	    (header.memory_type_mask & ~parent->memory_type_mask) != 0u ||
	    (header.memory_type_mask & ~MEMORY_TYPE_VALID_MASK) != 0u ||
	    ((header.flags & MEMORY_ALLOCATOR_DERIVE_INHERIT_CLAIMS) != 0u && header.claim_range_count != 0u))
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	child = calloc(1u, sizeof(*child));
	if (child == NULL) return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	child->reference_count  = 1u;
	child->memory_rights    = header.memory_rights;
	child->memory_type_mask = header.memory_type_mask;
	if (!allocator_retain(parent)) goto failed;
	child->parent = parent;
	if ((header.allocator_rights & CAP_MANAGE) != 0u && (header.flags & MEMORY_ALLOCATOR_DERIVE_INHERIT_CLAIMS) != 0u) {
		child->unrestricted_physical_claims = parent->unrestricted_physical_claims;
		if (!normalize_ranges(
				parent->claim_ranges, parent->claim_range_count, &child->claim_ranges, &child->claim_range_count))
			goto failed;
	}
	else if ((header.allocator_rights & CAP_MANAGE) != 0u) {
		const struct memory_allocator_physical_range* ranges =
			(const struct memory_allocator_physical_range*)((const uint8_t*)req->request + sizeof(header));
		for (size_t i = 0u; i < header.claim_range_count; i++)
			if (!range_allowed(parent, ranges[i])) goto bad_request;
		if (!normalize_ranges(ranges, header.claim_range_count, &child->claim_ranges, &child->claim_range_count))
			goto failed;
	}
	parent_cap = cap_acquire(req->cap_id);
	if (parent_cap == NULL) goto failed;
	response.allocator_cap = allocator_publish(child, req->caller, header.allocator_rights, parent_cap);
	cap_release(parent_cap);
	if (response.allocator_cap == CAP_ID_INVALID) return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	syscall_result_t result = cap_kernel_write_response(req, &response, sizeof(response));
	if (result.status != SYSCALL_STATUS_OK) (void)cap_destroy_by_id(response.allocator_cap);
	return result;
bad_request:
	allocator_release(child);
	return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
failed:
	allocator_release(child);
	return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
}

static syscall_result_t allocator_handler(const struct cap_request* req) {
	struct memory_allocator_request_header header;
	if (req == NULL || req->object_id == 0u || req->request == NULL || req->request_size < sizeof(header))
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	memcpy(&header, req->request, sizeof(header));
	struct kernel_memory_allocator* allocator = (struct kernel_memory_allocator*)(uintptr_t)req->object_id;
	switch (header.op) {
	case MEMORY_ALLOCATOR_OP_INFO:
		return allocator_info(req, allocator);
	case MEMORY_ALLOCATOR_OP_DERIVE:
		return allocator_derive(req, allocator);
	case MEMORY_ALLOCATOR_OP_ALLOC:
		return allocator_alloc(req, allocator);
	case MEMORY_ALLOCATOR_OP_CLAIM_PHYSICAL:
		return allocator_claim(req, allocator);
	default:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
}

bool kernel_memory_allocator_init(void) {
	if (root_allocator != NULL) return true;
	root_allocator = calloc(1u, sizeof(*root_allocator));
	if (root_allocator == NULL) return false;
	root_allocator->reference_count              = 1u;
	root_allocator->memory_rights                = ALLOCATOR_MEMORY_RIGHTS;
	root_allocator->memory_type_mask             = MEMORY_TYPE_VALID_MASK;
	root_allocator->unrestricted_physical_claims = true;
	return true;
}

cap_id_t kernel_memory_allocator_grant_root(process_id_t recipient) {
	if (!allocator_retain(root_allocator)) return CAP_ID_INVALID;
	return allocator_publish(root_allocator, recipient, ALLOCATOR_CAP_RIGHTS, NULL);
}
