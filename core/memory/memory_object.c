#include <base/math.h>
#include <base/vmm.h>
#include <core/memory_object.h>
#include <core/mm.h>
#include <core/pmm.h>
#include <hal/hcf.h>
#include <string.h>

#include "memory_object_radix.h"

struct object_free_slot {
	struct object_free_slot* next;
};
struct object_slab {
	struct object_slab*      next;
	struct object_free_slot* free_slots;
	uintptr_t                phys;
	size_t                   used;
};

static struct memory_object* external_claims;
static struct spinlock       external_claim_lock =
	SPINLOCK_INIT_CLASS("memory_external_claim", SPINLOCK_ORDER_MEMORY_OBJECT, SPINLOCK_FLAG_IRQSAVE);
static struct object_slab* object_slabs;
static struct spinlock     object_allocator_lock =
	SPINLOCK_INIT_CLASS("memory_object_allocator", SPINLOCK_ORDER_MEMORY_OBJECT, SPINLOCK_FLAG_IRQSAVE);

#define OBJECT_ALIGN _Alignof(struct memory_object)
#define OBJECT_SLOT_SIZE ((sizeof(struct memory_object) + OBJECT_ALIGN - 1u) & ~(OBJECT_ALIGN - 1u))
#define OBJECT_SLAB_OFFSET ((sizeof(struct object_slab) + OBJECT_ALIGN - 1u) & ~(OBJECT_ALIGN - 1u))

_Static_assert(OBJECT_SLAB_OFFSET + OBJECT_SLOT_SIZE <= VMM_PAGE_SIZE, "object slab must contain an object");

static void* phys_to_virt(uintptr_t phys) {
	return (void*)(uintptr_t)(phys + boot_info.direct_map_offset);
}

static struct object_slab* slab_create(void) {
	struct pmm_extent   allocation;
	struct object_slab* slab;
	uint8_t*            slots;
	size_t              count;
	if (!pmm_alloc(&(const struct pmm_alloc_request){.size = VMM_PAGE_SIZE, .alignment = VMM_PAGE_SIZE}, &allocation))
		return NULL;
	slab = phys_to_virt(allocation.address);
	memset(slab, 0, VMM_PAGE_SIZE);
	slab->phys = allocation.address;
	slots      = (uint8_t*)slab + OBJECT_SLAB_OFFSET;
	count      = (VMM_PAGE_SIZE - OBJECT_SLAB_OFFSET) / OBJECT_SLOT_SIZE;
	for (size_t i = 0u; i < count; i++) {
		struct object_free_slot* slot = (void*)(slots + i * OBJECT_SLOT_SIZE);
		slot->next                    = slab->free_slots;
		slab->free_slots              = slot;
	}
	return slab;
}

static struct object_slab* object_slab(const struct memory_object* object) {
	return (struct object_slab*)((uintptr_t)object & ~(uintptr_t)(VMM_PAGE_SIZE - 1u));
}

static struct memory_object* control_alloc(void) {
	struct irq_state      state = spinlock_lock_irqsave(&object_allocator_lock);
	struct object_slab*   slab;
	struct memory_object* object;
	for (slab = object_slabs; slab != NULL && slab->free_slots == NULL; slab = slab->next) {
	}
	if (slab == NULL) {
		slab = slab_create();
		if (slab == NULL) {
			spinlock_unlock_irqrestore(&object_allocator_lock, state);
			return NULL;
		}
		slab->next   = object_slabs;
		object_slabs = slab;
	}
	object           = (struct memory_object*)slab->free_slots;
	slab->free_slots = slab->free_slots->next;
	slab->used++;
	memset(object, 0, sizeof(*object));
	spinlock_unlock_irqrestore(&object_allocator_lock, state);
	return object;
}

static void control_free(struct memory_object* object) {
	struct object_slab*      slab = object_slab(object);
	struct object_slab**     link;
	struct object_free_slot* slot;
	struct irq_state         state = spinlock_lock_irqsave(&object_allocator_lock);
	if (slab->used == 0u) hcf();
	memset(object, 0, sizeof(*object));
	slot             = (struct object_free_slot*)object;
	slot->next       = slab->free_slots;
	slab->free_slots = slot;
	slab->used--;
	if (slab->used == 0u) {
		for (link = &object_slabs; *link != NULL && *link != slab; link = &(*link)->next) {
		}
		if (*link != slab) hcf();
		*link = slab->next;
		(void)pmm_free((struct pmm_extent){.address = slab->phys, .size = VMM_PAGE_SIZE});
	}
	spinlock_unlock_irqrestore(&object_allocator_lock, state);
}

bool memory_object_create_params_valid(const struct memory_create_params* params) {
	const struct memory_constraints* constraints;
	size_t                           align_pages;
	size_t                           align_bytes;
	uint64_t                         span;
	uint64_t                         fixed_end;

	if (params == NULL || params->page_count == 0u || params->page_count > SIZE_MAX / VMM_PAGE_SIZE ||
	    params->memory_type >= MEMORY_TYPE_COUNT)
		return false;
	constraints = &params->constraints;
	if (params->memory_type != MEMORY_TYPE_NORMAL && (constraints->flags & MEMORY_CONSTRAINT_FIXED) == 0u) return false;
	if ((constraints->flags & ~(uint32_t)(MEMORY_CONSTRAINT_CONTIGUOUS | MEMORY_CONSTRAINT_FIXED)) != 0u ||
	    (constraints->physical_min & (VMM_PAGE_SIZE - 1u)) != 0u ||
	    (constraints->physical_max != 0u && ((constraints->physical_max & (VMM_PAGE_SIZE - 1u)) != 0u ||
	                                         constraints->physical_max <= constraints->physical_min)))
		return false;
	align_pages = constraints->align_pages == 0u ? 1u : constraints->align_pages;
	if ((align_pages & (align_pages - 1u)) != 0u || mul_overflow_size(align_pages, VMM_PAGE_SIZE, &align_bytes) ||
	    mul_overflow_u64((uint64_t)params->page_count, VMM_PAGE_SIZE, &span))
		return false;
	if ((constraints->flags & MEMORY_CONSTRAINT_FIXED) == 0u) {
		if (constraints->physical_address != 0u ||
		    (align_pages > 1u && (constraints->flags & MEMORY_CONSTRAINT_CONTIGUOUS) == 0u))
			return false;
		if ((constraints->flags & MEMORY_CONSTRAINT_CONTIGUOUS) != 0u && constraints->physical_max != 0u &&
		    span > (uint64_t)constraints->physical_max - (uint64_t)constraints->physical_min)
			return false;
		return true;
	}
	if ((constraints->physical_address & (VMM_PAGE_SIZE - 1u)) != 0u ||
	    (constraints->physical_address & (align_bytes - 1u)) != 0u ||
	    constraints->physical_address < constraints->physical_min ||
	    add_overflow_u64((uint64_t)constraints->physical_address, span, &fixed_end) || fixed_end > UINTPTR_MAX ||
	    (constraints->physical_max != 0u && fixed_end > constraints->physical_max))
		return false;
	return true;
}

static bool object_create(enum memory_object_type type, const struct memory_create_params* params,
                          struct memory_object** out_object) {
	struct memory_object* object;
	if (out_object != NULL) *out_object = NULL;
	if (out_object == NULL || params == NULL) return false;
	object = control_alloc();
	if (object == NULL) return false;
	object->page_count      = params->page_count;
	object->reference_count = 1u;
	object->type            = (uint8_t)type;
	object->memory_type     = (uint8_t)params->memory_type;
	object->radix_depth     = type == MEMORY_OBJECT_OWNED ? memory_object_radix_depth(params->page_count) : 0u;
	spinlock_init_class(&object->lock,
	                    "memory_object",
	                    SPINLOCK_ORDER_MEMORY_OBJECT,
	                    SPINLOCK_FLAG_IRQSAVE | SPINLOCK_FLAG_ALLOW_EXCEPTION);
	*out_object = object;
	return true;
}

static bool external_claim(struct memory_object* object) {
	uint64_t         span;
	uint64_t         end;
	struct irq_state state;

	if (object == NULL || mul_overflow_u64((uint64_t)object->page_count, VMM_PAGE_SIZE, &span) ||
	    add_overflow_u64((uint64_t)object->backing_root_or_phys, span, &end))
		return false;
	state = spinlock_lock_irqsave(&external_claim_lock);
	for (struct memory_object* current = external_claims; current != NULL; current = current->claim_next) {
		uint64_t current_span;
		uint64_t current_end;
		if (mul_overflow_u64((uint64_t)current->page_count, VMM_PAGE_SIZE, &current_span) ||
		    add_overflow_u64((uint64_t)current->backing_root_or_phys, current_span, &current_end) ||
		    ((uint64_t)object->backing_root_or_phys < current_end && (uint64_t)current->backing_root_or_phys < end)) {
			spinlock_unlock_irqrestore(&external_claim_lock, state);
			return false;
		}
	}
	object->claim_next       = external_claims;
	object->external_claimed = true;
	external_claims          = object;
	spinlock_unlock_irqrestore(&external_claim_lock, state);
	return true;
}

static void external_unclaim(struct memory_object* object) {
	struct irq_state       state = spinlock_lock_irqsave(&external_claim_lock);
	struct memory_object** link  = &external_claims;
	while (*link != NULL && *link != object) link = &(*link)->claim_next;
	if (*link != object) hcf();
	*link                    = object->claim_next;
	object->claim_next       = NULL;
	object->external_claimed = false;
	spinlock_unlock_irqrestore(&external_claim_lock, state);
}

static void zero_pages(uintptr_t phys, size_t page_count) {
	memset(phys_to_virt(phys), 0, page_count * VMM_PAGE_SIZE);
}

bool memory_object_create(const struct memory_create_params* params, struct memory_object** out_object) {
	const struct memory_constraints* constraints;
	struct memory_object*            object;
	uintptr_t                        phys;
	bool                             contiguous;

	if (out_object != NULL) *out_object = NULL;
	if (out_object == NULL || !memory_object_create_params_valid(params)) return false;
	constraints = &params->constraints;
	contiguous  = (constraints->flags & (MEMORY_CONSTRAINT_CONTIGUOUS | MEMORY_CONSTRAINT_FIXED)) != 0u;

	if ((constraints->flags & MEMORY_CONSTRAINT_FIXED) != 0u) {
		struct pmm_extent extent = {
			.address = constraints->physical_address,
			.size    = params->page_count * VMM_PAGE_SIZE,
		};
		enum pmm_claim_result claim = pmm_claim(extent);
		if (claim == PMM_CLAIM_OK) {
			if (params->memory_type != MEMORY_TYPE_NORMAL) {
				(void)pmm_free(extent);
				return false;
			}
			if (!object_create(MEMORY_OBJECT_CONTIGUOUS, params, &object)) {
				(void)pmm_free(extent);
				return false;
			}
			object->backing_root_or_phys = constraints->physical_address;
			zero_pages(object->backing_root_or_phys, object->page_count);
			*out_object = object;
			return true;
		}
		if (claim != PMM_CLAIM_NOT_MANAGED || !object_create(MEMORY_OBJECT_EXTERNAL, params, &object)) return false;
		object->backing_root_or_phys = constraints->physical_address;
		if (!external_claim(object)) {
			control_free(object);
			return false;
		}
		*out_object = object;
		return true;
	}

	if (contiguous) {
		struct pmm_extent allocation;
		if (!pmm_alloc(
				&(const struct pmm_alloc_request){
					.size            = params->page_count * VMM_PAGE_SIZE,
					.alignment       = (constraints->align_pages == 0u ? 1u : constraints->align_pages) * VMM_PAGE_SIZE,
					.minimum_address = constraints->physical_min,
					.maximum_address = constraints->physical_max,
				},
				&allocation))
			return false;
		phys = allocation.address;
		if (!object_create(MEMORY_OBJECT_CONTIGUOUS, params, &object)) {
			(void)pmm_free(allocation);
			return false;
		}
		object->backing_root_or_phys = phys;
		zero_pages(phys, params->page_count);
		*out_object = object;
		return true;
	}

	if (!object_create(MEMORY_OBJECT_OWNED, params, &object)) return false;
	if (constraints->physical_min != 0u || constraints->physical_max != 0u) {
		for (size_t page = 0u; page < params->page_count; page++) {
			struct pmm_extent allocation;
			if (!pmm_alloc(
					&(const struct pmm_alloc_request){
						.size            = VMM_PAGE_SIZE,
						.alignment       = VMM_PAGE_SIZE,
						.minimum_address = constraints->physical_min,
						.maximum_address = constraints->physical_max,
					},
					&allocation)) {
				memory_object_release(object);
				return false;
			}
			phys = allocation.address;
			zero_pages(phys, 1u);
			if (!memory_object_radix_insert(object, page, phys)) {
				(void)pmm_free(allocation);
				memory_object_release(object);
				return false;
			}
		}
	}
	*out_object = object;
	return true;
}

bool memory_object_create_owned(size_t page_count, struct memory_object** out_object) {
	const struct memory_create_params params = {.page_count = page_count, .memory_type = MEMORY_TYPE_NORMAL};
	return memory_object_create(&params, out_object);
}

bool memory_object_create_external(uintptr_t phys_base, size_t page_count, struct memory_object** out_object) {
	struct memory_object*             object;
	uint64_t                          span;
	const struct memory_create_params params = {.page_count = page_count, .memory_type = MEMORY_TYPE_NORMAL};
	if (out_object != NULL) *out_object = NULL;
	if ((phys_base & (VMM_PAGE_SIZE - 1u)) != 0u || page_count == 0u ||
	    mul_overflow_u64((uint64_t)page_count, VMM_PAGE_SIZE, &span) || span - 1u > UINTPTR_MAX - phys_base)
		return false;
	if (!object_create(MEMORY_OBJECT_EXTERNAL, &params, &object)) return false;
	object->backing_root_or_phys = phys_base;
	*out_object                  = object;
	return true;
}

bool memory_object_retain(struct memory_object* object) {
	uint64_t current;
	if (object == NULL) return false;
	current = __atomic_load_n(&object->reference_count, __ATOMIC_ACQUIRE);
	for (;;) {
		if (current == 0u || current == UINT64_MAX) return false;
		if (__atomic_compare_exchange_n(
				&object->reference_count, &current, current + 1u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
			return true;
	}
}

void memory_object_release(struct memory_object* object) {
	uint64_t old;
	if (object == NULL) return;
	old = __atomic_fetch_sub(&object->reference_count, 1u, __ATOMIC_ACQ_REL);
	if (old == 0u) hcf();
	if (old != 1u) return;
	if (object->type == MEMORY_OBJECT_OWNED) memory_object_radix_release(object);
	else if (object->type == MEMORY_OBJECT_CONTIGUOUS)
		(void)pmm_free((struct pmm_extent){
			.address = object->backing_root_or_phys,
			.size    = object->page_count * VMM_PAGE_SIZE,
		});
	else if (object->external_claimed) external_unclaim(object);
	control_free(object);
}

enum memory_object_type memory_object_type(const struct memory_object* object) {
	return object == NULL ? MEMORY_OBJECT_OWNED : (enum memory_object_type)object->type;
}

enum memory_type memory_object_memory_type(const struct memory_object* object) {
	return object == NULL ? MEMORY_TYPE_NORMAL : (enum memory_type)object->memory_type;
}

bool memory_object_can_transfer(const struct memory_object* object) {
	return object != NULL && memory_object_memory_type(object) == MEMORY_TYPE_NORMAL &&
	       !(object->type == MEMORY_OBJECT_EXTERNAL && object->external_claimed);
}

size_t memory_object_page_count(const struct memory_object* object) {
	return object == NULL ? 0u : object->page_count;
}

bool memory_object_page_phys(struct memory_object* object, size_t logical_page, uintptr_t* out_phys) {
	struct irq_state state;
	bool             found;
	if (out_phys != NULL) *out_phys = 0u;
	if (object == NULL || out_phys == NULL || logical_page >= object->page_count) return false;
	state = spinlock_lock_irqsave(&object->lock);
	if (object->type != MEMORY_OBJECT_OWNED) {
		*out_phys = object->backing_root_or_phys + logical_page * (uintptr_t)VMM_PAGE_SIZE;
		found     = true;
	}
	else found = memory_object_radix_lookup(object, logical_page, out_phys);
	spinlock_unlock_irqrestore(&object->lock, state);
	return found;
}

bool memory_object_resolve_page(struct memory_object* object, size_t logical_page, uintptr_t* out_phys) {
	struct irq_state state;
	bool             ok;
	if (out_phys != NULL) *out_phys = 0u;
	if (object == NULL || out_phys == NULL || logical_page >= object->page_count) return false;
	state = spinlock_lock_irqsave(&object->lock);
	if (object->type != MEMORY_OBJECT_OWNED) {
		*out_phys = object->backing_root_or_phys + logical_page * (uintptr_t)VMM_PAGE_SIZE;
		ok        = true;
	}
	else ok = memory_object_radix_resolve(object, logical_page, out_phys);
	spinlock_unlock_irqrestore(&object->lock, state);
	return ok;
}

static bool access_bounds(const struct memory_object* object, size_t offset, size_t size) {
	size_t bytes;
	return object != NULL && !mul_overflow_size(object->page_count, VMM_PAGE_SIZE, &bytes) && offset <= bytes &&
	       size <= bytes - offset;
}

bool memory_object_read(struct memory_object* object, size_t byte_offset, void* dst, size_t size) {
	struct irq_state state;
	size_t           done = 0u;
	if (size == 0u) return access_bounds(object, byte_offset, 0u);
	if (dst == NULL || !access_bounds(object, byte_offset, size)) return false;
	state = spinlock_lock_irqsave(&object->lock);
	while (done < size) {
		size_t    offset = byte_offset + done, page = offset / VMM_PAGE_SIZE;
		size_t    within = offset & (VMM_PAGE_SIZE - 1u), chunk = VMM_PAGE_SIZE - within;
		uintptr_t phys;
		if (chunk > size - done) chunk = size - done;
		if (object->type != MEMORY_OBJECT_OWNED) {
			if (!memory_object_can_transfer(object)) {
				spinlock_unlock_irqrestore(&object->lock, state);
				return false;
			}
			phys = object->backing_root_or_phys + page * VMM_PAGE_SIZE;
		}
		else if (!memory_object_radix_lookup(object, page, &phys)) {
			memset((uint8_t*)dst + done, 0, chunk);
			done += chunk;
			continue;
		}
		memcpy((uint8_t*)dst + done, (uint8_t*)phys_to_virt(phys) + within, chunk);
		done += chunk;
	}
	spinlock_unlock_irqrestore(&object->lock, state);
	return true;
}

bool memory_object_write(struct memory_object* object, size_t byte_offset, const void* src, size_t size) {
	struct irq_state state;
	size_t           done = 0u;
	if (size == 0u) return access_bounds(object, byte_offset, 0u);
	if (src == NULL || !access_bounds(object, byte_offset, size)) return false;
	state = spinlock_lock_irqsave(&object->lock);
	while (done < size) {
		size_t    offset = byte_offset + done, page = offset / VMM_PAGE_SIZE;
		size_t    within = offset & (VMM_PAGE_SIZE - 1u), chunk = VMM_PAGE_SIZE - within;
		uintptr_t phys;
		if (chunk > size - done) chunk = size - done;
		if (object->type != MEMORY_OBJECT_OWNED) {
			if (!memory_object_can_transfer(object)) {
				spinlock_unlock_irqrestore(&object->lock, state);
				return false;
			}
			phys = object->backing_root_or_phys + page * VMM_PAGE_SIZE;
		}
		else if (!memory_object_radix_resolve(object, page, &phys)) {
			spinlock_unlock_irqrestore(&object->lock, state);
			return false;
		}
		memcpy((uint8_t*)phys_to_virt(phys) + within, (const uint8_t*)src + done, chunk);
		done += chunk;
	}
	spinlock_unlock_irqrestore(&object->lock, state);
	return true;
}
