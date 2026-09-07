#include <base/math.h>
#include <core/memory_backing.h>
#include <core/mm.h>
#include <core/pmm.h>
#include <core/spinlock.h>
#include <hal/hcf.h>
#include <stddef.h>
#include <string.h>

#include "memory_backing_tree.h"

struct memory_backing {
	struct spinlock               lock;
	struct memory_backing_rb_tree extents;
	struct memory_backing_rb_node claim_node;
	size_t                        size;
	uintptr_t                     claim_address;
	uint64_t                      reference_count;
	uint64_t                      topology_generation;
	enum memory_backing_kind      kind;
	bool                          cpu_accessible;
	bool                          external_claim;
};

union backing_metadata_payload {
	struct memory_backing        backing;
	struct memory_backing_extent extent;
};

struct backing_metadata_slot {
	struct backing_metadata_slot*  next;
	union backing_metadata_payload payload;
};

struct backing_metadata_slab {
	struct backing_metadata_slab* next;
	struct backing_metadata_slab* previous;
	struct backing_metadata_slot* free_slots;
	uintptr_t                     physical_address;
	size_t                        used;
};

static struct backing_metadata_slab* metadata_slabs;
static struct spinlock               metadata_lock =
	SPINLOCK_INIT_CLASS("memory_backing_metadata", SPINLOCK_ORDER_BACKING_METADATA, SPINLOCK_FLAG_IRQSAVE);
static struct memory_backing_rb_tree external_claims;
static struct spinlock               external_claim_lock =
	SPINLOCK_INIT_CLASS("memory_backing_claim", SPINLOCK_ORDER_MEMORY_BACKING, SPINLOCK_FLAG_IRQSAVE);

#define METADATA_ALIGN _Alignof(struct backing_metadata_slot)
#define METADATA_SLAB_OFFSET ((sizeof(struct backing_metadata_slab) + METADATA_ALIGN - 1u) & ~(METADATA_ALIGN - 1u))

static void* physical_to_virtual(uintptr_t physical_address) {
	return (void*)(physical_address + boot_info.direct_map_offset);
}

static size_t backing_granule(void) {
	const struct pmm_info* info = pmm_info();
	return info == NULL ? 0u : info->allocation_granule;
}

static struct backing_metadata_slab* metadata_slab_create(size_t granule) {
	struct pmm_extent             allocation;
	struct backing_metadata_slab* slab;
	uint8_t*                      slots;
	size_t                        count;
	if (granule == 0u || !pmm_alloc(&(const struct pmm_alloc_request){.size = granule}, &allocation)) return NULL;
	slab = physical_to_virtual(allocation.address);
	memset(slab, 0, granule);
	if (granule <= METADATA_SLAB_OFFSET) {
		(void)pmm_free(allocation);
		return NULL;
	}
	count = (granule - METADATA_SLAB_OFFSET) / sizeof(struct backing_metadata_slot);
	if (count == 0u) {
		(void)pmm_free(allocation);
		return NULL;
	}
	slab->physical_address = allocation.address;
	slots                  = (uint8_t*)slab + METADATA_SLAB_OFFSET;
	for (size_t i = 0u; i < count; i++) {
		struct backing_metadata_slot* slot = (void*)(slots + i * sizeof(*slot));
		slot->next                         = slab->free_slots;
		slab->free_slots                   = slot;
	}
	return slab;
}

static void* metadata_alloc(void) {
	struct irq_state              state = spinlock_lock_irqsave(&metadata_lock);
	struct backing_metadata_slab* slab;
	struct backing_metadata_slot* slot;
	size_t                        granule = backing_granule();
	slab = metadata_slabs != NULL && metadata_slabs->free_slots != NULL ? metadata_slabs : NULL;
	if (slab == NULL) {
		slab = metadata_slab_create(granule);
		if (slab == NULL) {
			spinlock_unlock_irqrestore(&metadata_lock, state);
			return NULL;
		}
		slab->next = metadata_slabs;
		if (metadata_slabs != NULL) metadata_slabs->previous = slab;
		metadata_slabs = slab;
	}
	slot             = slab->free_slots;
	slab->free_slots = slot->next;
	slab->used++;
	memset(&slot->payload, 0, sizeof(slot->payload));
	spinlock_unlock_irqrestore(&metadata_lock, state);
	return &slot->payload;
}

static struct backing_metadata_slab* metadata_owner(const void* payload, size_t granule) {
	uintptr_t physical = (uintptr_t)payload - boot_info.direct_map_offset;
	physical &= ~(uintptr_t)(granule - 1u);
	return physical_to_virtual(physical);
}

static void metadata_free(void* payload) {
	struct backing_metadata_slot* slot;
	struct backing_metadata_slab* slab;
	struct irq_state              state;
	uintptr_t                     physical_address = 0u;
	size_t                        granule          = backing_granule();
	bool                          release          = false;
	if (payload == NULL || granule == 0u) hcf();
	slot  = (struct backing_metadata_slot*)((uint8_t*)payload - offsetof(struct backing_metadata_slot, payload));
	slab  = metadata_owner(payload, granule);
	state = spinlock_lock_irqsave(&metadata_lock);
	if (slab->used == 0u) hcf();
	memset(&slot->payload, 0, sizeof(slot->payload));
	slot->next       = slab->free_slots;
	slab->free_slots = slot;
	slab->used--;
	if (slab->previous != NULL) slab->previous->next = slab->next;
	else if (metadata_slabs == slab) metadata_slabs = slab->next;
	else hcf();
	if (slab->next != NULL) slab->next->previous = slab->previous;
	if (slab->used == 0u) {
		physical_address = slab->physical_address;
		release          = true;
	}
	else {
		slab->previous = NULL;
		slab->next     = metadata_slabs;
		if (metadata_slabs != NULL) metadata_slabs->previous = slab;
		metadata_slabs = slab;
	}
	spinlock_unlock_irqrestore(&metadata_lock, state);
	if (release && !pmm_free((struct pmm_extent){.address = physical_address, .size = granule})) hcf();
}

static struct memory_backing* claim_from_node(struct memory_backing_rb_node* node) {
	return node == NULL ? NULL : (struct memory_backing*)((uint8_t*)node - offsetof(struct memory_backing, claim_node));
}

static bool external_claim_insert(struct memory_backing* backing) {
	struct memory_backing_rb_node*  parent = NULL;
	struct memory_backing_rb_node** link   = &external_claims.root;
	struct irq_state                state;
	uintptr_t                       end = backing->claim_address + backing->size;
	state                               = spinlock_lock_irqsave(&external_claim_lock);
	while (*link != NULL) {
		struct memory_backing* current     = claim_from_node(*link);
		uintptr_t              current_end = current->claim_address + current->size;
		parent                             = *link;
		if (end <= current->claim_address) link = &(*link)->left;
		else if (backing->claim_address >= current_end) link = &(*link)->right;
		else {
			spinlock_unlock_irqrestore(&external_claim_lock, state);
			return false;
		}
	}
	memory_backing_rb_insert(&external_claims, &backing->claim_node, parent, link);
	backing->external_claim = true;
	spinlock_unlock_irqrestore(&external_claim_lock, state);
	return true;
}

static void external_claim_remove(struct memory_backing* backing) {
	struct irq_state state = spinlock_lock_irqsave(&external_claim_lock);
	if (!backing->external_claim) hcf();
	memory_backing_rb_remove(&external_claims, &backing->claim_node);
	backing->external_claim = false;
	spinlock_unlock_irqrestore(&external_claim_lock, state);
}

static bool valid_capacity(size_t size) {
	size_t granule = backing_granule();
	return granule != 0u && (granule & (granule - 1u)) == 0u && size != 0u && (size & (granule - 1u)) == 0u;
}

static struct memory_backing* backing_alloc(size_t size, enum memory_backing_kind kind) {
	struct memory_backing* backing = metadata_alloc();
	if (backing == NULL) return NULL;
	backing->size            = size;
	backing->kind            = kind;
	backing->reference_count = 1u;
	spinlock_init_class(&backing->lock,
	                    "memory_backing",
	                    SPINLOCK_ORDER_MEMORY_BACKING,
	                    SPINLOCK_FLAG_IRQSAVE | SPINLOCK_FLAG_ALLOW_EXCEPTION);
	return backing;
}

bool memory_backing_create_anonymous(size_t size, struct memory_backing** out_backing) {
	struct memory_backing* backing;
	if (out_backing != NULL) *out_backing = NULL;
	if (out_backing == NULL || !valid_capacity(size)) return false;
	backing = backing_alloc(size, MEMORY_BACKING_ANONYMOUS);
	if (backing == NULL) return false;
	backing->cpu_accessible = true;
	*out_backing            = backing;
	return true;
}

bool memory_backing_create_physical(const struct memory_backing_physical_request* request,
                                    struct memory_backing**                       out_backing) {
	struct memory_backing*        backing;
	struct memory_backing_extent* extent;
	enum pmm_claim_result         claim;
	size_t                        granule = backing_granule();
	if (out_backing != NULL) *out_backing = NULL;
	if (out_backing == NULL || request == NULL || !valid_capacity(request->size) ||
	    (request->physical_address & (granule - 1u)) != 0u || request->size > UINTPTR_MAX - request->physical_address)
		return false;
	claim = pmm_claim((struct pmm_extent){.address = request->physical_address, .size = request->size});
	if (claim == PMM_CLAIM_UNAVAILABLE) return false;
	backing = backing_alloc(request->size, MEMORY_BACKING_PHYSICAL);
	if (backing == NULL) {
		if (claim == PMM_CLAIM_OK &&
		    !pmm_free((struct pmm_extent){.address = request->physical_address, .size = request->size}))
			hcf();
		return false;
	}
	extent = metadata_alloc();
	if (extent == NULL) {
		if (claim == PMM_CLAIM_OK &&
		    !pmm_free((struct pmm_extent){.address = request->physical_address, .size = request->size}))
			hcf();
		metadata_free(backing);
		return false;
	}
	*extent = (struct memory_backing_extent){
		.logical_start  = 0u,
		.size           = request->size,
		.physical_start = request->physical_address,
	};
	backing->claim_address = request->physical_address;
	if (claim == PMM_CLAIM_OK) {
		extent->source          = MEMORY_BACKING_EXTENT_PMM;
		backing->cpu_accessible = true;
	}
	else if (claim == PMM_CLAIM_NOT_MANAGED) {
		extent->source          = MEMORY_BACKING_EXTENT_EXTERNAL;
		backing->cpu_accessible = request->external_cpu_accessible;
		if (!external_claim_insert(backing)) {
			metadata_free(extent);
			metadata_free(backing);
			return false;
		}
	}
	else if (claim != PMM_CLAIM_NOT_MANAGED) hcf();
	if (!memory_backing_tree_insert(&backing->extents, extent)) hcf();
	*out_backing = backing;
	return true;
}

bool memory_backing_retain(struct memory_backing* backing) {
	uint64_t current;
	if (backing == NULL) return false;
	current = __atomic_load_n(&backing->reference_count, __ATOMIC_ACQUIRE);
	for (;;) {
		if (current == 0u || current == UINT64_MAX) return false;
		if (__atomic_compare_exchange_n(
				&backing->reference_count, &current, current + 1u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
			return true;
	}
}

static void release_extent_tree(struct memory_backing_rb_node* node) {
	struct memory_backing_extent* extent;
	if (node == NULL) return;
	release_extent_tree(node->left);
	release_extent_tree(node->right);
	extent = (struct memory_backing_extent*)((uint8_t*)node - offsetof(struct memory_backing_extent, tree_node));
	if (extent->source == MEMORY_BACKING_EXTENT_PMM &&
	    !pmm_free((struct pmm_extent){.address = extent->physical_start, .size = extent->size}))
		hcf();
	metadata_free(extent);
}

void memory_backing_release(struct memory_backing* backing) {
	uint64_t old;
	if (backing == NULL) return;
	old = __atomic_fetch_sub(&backing->reference_count, 1u, __ATOMIC_ACQ_REL);
	if (old == 0u) hcf();
	if (old != 1u) return;
	if (backing->external_claim) external_claim_remove(backing);
	release_extent_tree(backing->extents.root);
	metadata_free(backing);
}

size_t memory_backing_size(const struct memory_backing* backing) {
	return backing == NULL ? 0u : backing->size;
}

enum memory_backing_kind memory_backing_kind(const struct memory_backing* backing) {
	return backing == NULL ? MEMORY_BACKING_ANONYMOUS : backing->kind;
}

bool memory_backing_cpu_accessible(const struct memory_backing* backing) {
	return backing != NULL && backing->cpu_accessible;
}

static bool valid_range(const struct memory_backing* backing, size_t offset, size_t size) {
	return backing != NULL && offset <= backing->size && size <= backing->size - offset;
}

static void query_locked(struct memory_backing* backing, size_t offset, size_t maximum_size,
                         struct memory_backing_span* span) {
	struct memory_backing_extent* extent = memory_backing_tree_find(&backing->extents, offset);
	span->offset                         = offset;
	span->physical_address               = 0u;
	if (extent != NULL) {
		size_t within          = offset - extent->logical_start;
		span->kind             = MEMORY_BACKING_SPAN_PRESENT;
		span->size             = extent->size - within;
		span->physical_address = extent->physical_start + within;
	}
	else {
		struct memory_backing_extent* next = memory_backing_tree_lower_bound(&backing->extents, offset);
		span->kind                         = MEMORY_BACKING_SPAN_HOLE;
		span->size                         = next == NULL ? backing->size - offset : next->logical_start - offset;
	}
	if (span->size > maximum_size) span->size = maximum_size;
}

bool memory_backing_query(struct memory_backing* backing, size_t offset, size_t maximum_size,
                          struct memory_backing_span* out_span) {
	struct irq_state state;
	if (out_span != NULL) *out_span = (struct memory_backing_span){0};
	if (out_span == NULL || maximum_size == 0u || !valid_range(backing, offset, maximum_size)) return false;
	state = spinlock_lock_irqsave(&backing->lock);
	query_locked(backing, offset, maximum_size, out_span);
	spinlock_unlock_irqrestore(&backing->lock, state);
	return true;
}

static bool extents_compatible(const struct memory_backing_extent* left, const struct memory_backing_extent* right) {
	return left != NULL && right != NULL && left->source == right->source &&
	       left->logical_start + left->size == right->logical_start &&
	       left->physical_start + left->size == right->physical_start;
}

static void insert_and_merge(struct memory_backing* backing, struct memory_backing_extent* extent) {
	struct memory_backing_extent* neighbor;
	if (!memory_backing_tree_insert(&backing->extents, extent)) hcf();
	neighbor = memory_backing_tree_previous(extent);
	if (extents_compatible(neighbor, extent)) {
		neighbor->size += extent->size;
		memory_backing_tree_remove(&backing->extents, extent);
		metadata_free(extent);
		extent = neighbor;
	}
	neighbor = memory_backing_tree_next(extent);
	if (extents_compatible(extent, neighbor)) {
		extent->size += neighbor->size;
		memory_backing_tree_remove(&backing->extents, neighbor);
		metadata_free(neighbor);
	}
}

static void rollback_pending(struct memory_backing_extent* pending) {
	while (pending != NULL) {
		struct memory_backing_extent* next = pending->pending_next;
		if (!pmm_free((struct pmm_extent){.address = pending->physical_start, .size = pending->size})) hcf();
		metadata_free(pending);
		pending = next;
	}
}

struct materialize_plan {
	size_t                   logical_start;
	size_t                   size;
	struct pmm_alloc_request allocation;
	bool                     allow_smaller;
};

static bool allocate_plan(struct memory_backing_extent** pending, const struct materialize_plan* plan) {
	size_t logical_start = plan->logical_start;
	size_t size          = plan->size;
	while (size != 0u) {
		struct memory_backing_extent* extent;
		struct pmm_alloc_request      request = plan->allocation;
		struct pmm_extent             allocation;
		size_t                        attempt = size;
		for (;;) {
			request.size = attempt;
			if (pmm_alloc(&request, &allocation)) break;
			if (!plan->allow_smaller || attempt == backing_granule()) return false;
			attempt = ((attempt / 2u) / backing_granule()) * backing_granule();
			if (attempt == 0u) attempt = backing_granule();
		}
		extent = metadata_alloc();
		if (extent == NULL) {
			if (!pmm_free(allocation)) hcf();
			return false;
		}
		memset(physical_to_virtual(allocation.address), 0, allocation.size);
		*extent = (struct memory_backing_extent){
			.logical_start  = logical_start,
			.size           = allocation.size,
			.physical_start = allocation.address,
			.source         = MEMORY_BACKING_EXTENT_PMM,
			.pending_next   = *pending,
		};
		*pending = extent;
		logical_start += allocation.size;
		size -= allocation.size;
	}
	return true;
}

static bool request_valid(struct memory_backing* backing, const struct memory_backing_materialize_request* request) {
	size_t granule = backing_granule();
	size_t alignment;
	if (backing == NULL || request == NULL || backing->kind != MEMORY_BACKING_ANONYMOUS || granule == 0u) return false;
	alignment = request->alignment == 0u ? granule : request->alignment;
	return request->size != 0u && (request->offset & (granule - 1u)) == 0u && (request->size & (granule - 1u)) == 0u &&
	       valid_range(backing, request->offset, request->size) && alignment >= granule &&
	       (alignment & (alignment - 1u)) == 0u &&
	       (request->maximum_address == 0u || request->maximum_address > request->minimum_address);
}

static bool contiguous_base_locked(struct memory_backing*                           backing,
                                   const struct memory_backing_materialize_request* request, uintptr_t* out_base,
                                   bool* out_whole_hole) {
	struct memory_backing_extent* anchor =
		memory_backing_tree_first_intersecting(&backing->extents, request->offset, request->size);
	if (anchor == NULL) {
		*out_whole_hole = true;
		return true;
	}
	*out_whole_hole = false;
	if (anchor->logical_start <= request->offset) {
		size_t within = request->offset - anchor->logical_start;
		if (within > UINTPTR_MAX - anchor->physical_start) return false;
		*out_base = anchor->physical_start + within;
	}
	else {
		size_t before = anchor->logical_start - request->offset;
		if (anchor->physical_start < before) return false;
		*out_base = anchor->physical_start - before;
	}
	return request->size <= UINTPTR_MAX - *out_base;
}

static bool next_noncontiguous_plan_locked(struct memory_backing*                           backing,
                                           const struct memory_backing_materialize_request* request, size_t* cursor,
                                           struct materialize_plan* plan) {
	size_t                        end = request->offset + request->size;
	struct memory_backing_extent* extent =
		memory_backing_tree_first_intersecting(&backing->extents, *cursor, end - *cursor);
	while (*cursor < end && extent != NULL && *cursor >= extent->logical_start) {
		size_t extent_end = extent->logical_start + extent->size;
		*cursor           = extent_end < end ? extent_end : end;
		extent            = memory_backing_tree_next(extent);
	}
	if (*cursor == end) {
		*plan = (struct materialize_plan){0};
		return true;
	}
	size_t hole_end = extent != NULL && extent->logical_start < end ? extent->logical_start : end;
	*plan           = (struct materialize_plan){
				  .logical_start = *cursor,
				  .size          = hole_end - *cursor,
				  .allocation =
            {
                         .alignment       = request->alignment,
                         .minimum_address = request->minimum_address,
                         .maximum_address = request->maximum_address,
						 },
				  .allow_smaller = true,
    };
	*cursor = hole_end;
	return true;
}

static bool next_contiguous_plan_locked(struct memory_backing*                           backing,
                                        const struct memory_backing_materialize_request* request,
                                        uintptr_t expected_base, size_t* cursor, struct materialize_plan* plan) {
	size_t                        end       = request->offset + request->size;
	size_t                        alignment = request->alignment == 0u ? backing_granule() : request->alignment;
	struct memory_backing_extent* extent =
		memory_backing_tree_first_intersecting(&backing->extents, *cursor, end - *cursor);
	while (*cursor < end && extent != NULL && *cursor >= extent->logical_start) {
		size_t    within     = *cursor - extent->logical_start;
		size_t    extent_end = extent->logical_start + extent->size;
		uintptr_t expected   = expected_base + (*cursor - request->offset);
		if (extent->physical_start + within != expected) return false;
		*cursor = extent_end < end ? extent_end : end;
		extent  = memory_backing_tree_next(extent);
	}
	if (*cursor == end) {
		*plan = (struct materialize_plan){0};
		return true;
	}
	size_t    hole_end  = extent != NULL && extent->logical_start < end ? extent->logical_start : end;
	size_t    hole_size = hole_end - *cursor;
	uintptr_t expected  = expected_base + (*cursor - request->offset);
	uintptr_t exact_end = expected + hole_size;
	if ((expected_base & (alignment - 1u)) != 0u || expected < request->minimum_address ||
	    (request->maximum_address != 0u && exact_end > request->maximum_address))
		return false;
	*plan = (struct materialize_plan){
		.logical_start = *cursor,
		.size          = hole_size,
		.allocation =
			{
						 .alignment       = backing_granule(),
						 .minimum_address = expected,
						 .maximum_address = exact_end,
						 },
	};
	*cursor = hole_end;
	return true;
}

static void commit_pending_locked(struct memory_backing* backing, struct memory_backing_extent* pending) {
	if (pending == NULL) return;
	while (pending != NULL) {
		struct memory_backing_extent* next = pending->pending_next;
		pending->pending_next              = NULL;
		insert_and_merge(backing, pending);
		pending = next;
	}
	if (backing->topology_generation == UINT64_MAX) hcf();
	backing->topology_generation++;
#if CORE_LOCK_DEBUG
	if (!memory_backing_tree_valid(&backing->extents)) hcf();
#endif
}

bool memory_backing_materialize(struct memory_backing*                           backing,
                                const struct memory_backing_materialize_request* request) {
	if (!request_valid(backing, request)) return false;
	for (;;) {
		struct memory_backing_extent* pending = NULL;
		struct materialize_plan       plan;
		struct irq_state              state;
		uint64_t                      generation;
		uintptr_t                     contiguous_base = 0u;
		size_t                        cursor          = request->offset;
		bool                          whole_hole      = false;
		bool                          retry           = false;
		state                                         = spinlock_lock_irqsave(&backing->lock);
		generation                                    = backing->topology_generation;
		if (request->require_contiguous && !contiguous_base_locked(backing, request, &contiguous_base, &whole_hole)) {
			spinlock_unlock_irqrestore(&backing->lock, state);
			return false;
		}
		spinlock_unlock_irqrestore(&backing->lock, state);
		if (whole_hole) {
			plan = (struct materialize_plan){
				.logical_start = request->offset,
				.size          = request->size,
				.allocation =
					{
								 .alignment       = request->alignment,
								 .minimum_address = request->minimum_address,
								 .maximum_address = request->maximum_address,
								 },
			};
			cursor = request->offset + request->size;
			if (!allocate_plan(&pending, &plan)) {
				state = spinlock_lock_irqsave(&backing->lock);
				retry = generation != backing->topology_generation;
				spinlock_unlock_irqrestore(&backing->lock, state);
				rollback_pending(pending);
				if (retry) continue;
				return false;
			}
		}
		while (cursor < request->offset + request->size) {
			bool plan_valid = true;
			state           = spinlock_lock_irqsave(&backing->lock);
			if (generation != backing->topology_generation) retry = true;
			else if (request->require_contiguous)
				plan_valid = next_contiguous_plan_locked(backing, request, contiguous_base, &cursor, &plan);
			else plan_valid = next_noncontiguous_plan_locked(backing, request, &cursor, &plan);
			spinlock_unlock_irqrestore(&backing->lock, state);
			if (retry) break;
			if (!plan_valid) {
				rollback_pending(pending);
				return false;
			}
			if (plan.size == 0u) break;
			if (!allocate_plan(&pending, &plan)) {
				state = spinlock_lock_irqsave(&backing->lock);
				retry = generation != backing->topology_generation;
				spinlock_unlock_irqrestore(&backing->lock, state);
				if (!retry) {
					rollback_pending(pending);
					return false;
				}
				break;
			}
		}
		if (!retry) {
			state = spinlock_lock_irqsave(&backing->lock);
			if (generation == backing->topology_generation) {
				commit_pending_locked(backing, pending);
				spinlock_unlock_irqrestore(&backing->lock, state);
				return true;
			}
			spinlock_unlock_irqrestore(&backing->lock, state);
		}
		rollback_pending(pending);
	}
}

bool memory_backing_read(struct memory_backing* backing, size_t offset, void* destination, size_t size) {
	struct memory_backing_span span;
	size_t                     end;
	if (size == 0u) return valid_range(backing, offset, 0u);
	if (destination == NULL || !valid_range(backing, offset, size) || !memory_backing_cpu_accessible(backing))
		return false;
	end = offset + size;
	for (size_t cursor = offset; cursor < end; cursor += span.size) {
		if (!memory_backing_query(backing, cursor, end - cursor, &span)) return false;
		if (span.kind == MEMORY_BACKING_SPAN_PRESENT)
			memcpy((uint8_t*)destination + (cursor - offset), physical_to_virtual(span.physical_address), span.size);
		else memset((uint8_t*)destination + (cursor - offset), 0, span.size);
	}
	return true;
}

bool memory_backing_write(struct memory_backing* backing, size_t offset, const void* source, size_t size) {
	struct memory_backing_span span;
	size_t                     granule = backing_granule();
	size_t                     materialize_start;
	size_t                     materialize_end;
	size_t                     end;
	if (size == 0u) return valid_range(backing, offset, 0u);
	if (source == NULL || granule == 0u || !valid_range(backing, offset, size) ||
	    !memory_backing_cpu_accessible(backing))
		return false;
	if (backing->kind == MEMORY_BACKING_ANONYMOUS) {
		materialize_start = offset & ~(granule - 1u);
		materialize_end   = (offset + size + granule - 1u) & ~(granule - 1u);
		if (!memory_backing_materialize(backing,
		                                &(const struct memory_backing_materialize_request){
											.offset = materialize_start,
											.size   = materialize_end - materialize_start,
										}))
			return false;
	}
	end = offset + size;
	for (size_t cursor = offset; cursor < end; cursor += span.size) {
		if (!memory_backing_query(backing, cursor, end - cursor, &span) || span.kind != MEMORY_BACKING_SPAN_PRESENT)
			return false;
		memcpy(physical_to_virtual(span.physical_address), (const uint8_t*)source + (cursor - offset), span.size);
	}
	return true;
}
