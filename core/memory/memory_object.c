#include <base/math.h>
#include <core/backing_store.h>
#include <core/memory_object.h>
#include <core/mm.h>
#include <core/pmm.h>
#include <core/spinlock.h>
#include <hal/hcf.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static struct memory_object_slab* object_slabs;
static bool                       object_allocator_initialized;
static struct spinlock            object_allocator_lock =
	SPINLOCK_INIT_CLASS("memory_object_allocator", SPINLOCK_ORDER_MEMORY_OBJECT, SPINLOCK_FLAG_IRQSAVE);

#define MEMORY_OBJECT_SLOT_ALIGN _Alignof(struct memory_object)
#define MEMORY_OBJECT_SLOT_SIZE                                                                                        \
	((sizeof(struct memory_object) + MEMORY_OBJECT_SLOT_ALIGN - 1u) & ~(MEMORY_OBJECT_SLOT_ALIGN - 1u))
#define MEMORY_OBJECT_SLAB_OFFSET                                                                                      \
	((sizeof(struct memory_object_slab) + MEMORY_OBJECT_SLOT_ALIGN - 1u) & ~(MEMORY_OBJECT_SLOT_ALIGN - 1u))

_Static_assert(MEMORY_OBJECT_SLAB_OFFSET + MEMORY_OBJECT_SLOT_SIZE <= PMM_PAGE_SIZE,
               "memory object slab must contain at least one object");

static inline void* hhdm_phys_to_virt(uintptr_t phys) {
	return (void*)(uintptr_t)(phys + boot_info.direct_map_offset);
}

static struct memory_object_slab* memory_object_slab_create(void) {
	struct memory_object_slab* slab;
	uintptr_t                  phys = 0u;
	uint8_t*                   slots;
	size_t                     slot_count;

	if (!pmm_alloc_pages(1u, &phys)) return NULL;
	slab = hhdm_phys_to_virt(phys);
	memset(slab, 0, PMM_PAGE_SIZE);
	slab->phys = phys;
	slots      = (uint8_t*)slab + MEMORY_OBJECT_SLAB_OFFSET;
	slot_count = (PMM_PAGE_SIZE - MEMORY_OBJECT_SLAB_OFFSET) / MEMORY_OBJECT_SLOT_SIZE;
	for (size_t slot = 0; slot < slot_count; slot++) {
		struct memory_object_free_slot* free_slot = (void*)(slots + slot * MEMORY_OBJECT_SLOT_SIZE);

		free_slot->next  = slab->free_slots;
		slab->free_slots = free_slot;
	}
	return slab;
}

bool memory_object_allocator_init(void) {
	struct memory_object_slab* slab;
	struct irq_state           state;

	state = spinlock_lock_irqsave(&object_allocator_lock);
	if (object_allocator_initialized) {
		for (slab = object_slabs; slab != NULL; slab = slab->next) {
			if (slab->used != 0u) {
				spinlock_unlock_irqrestore(&object_allocator_lock, state);
				return false;
			}
		}
		while (object_slabs != NULL) {
			slab         = object_slabs;
			object_slabs = slab->next;
			(void)pmm_free_pages(slab->phys, 1u);
		}
	}
	slab = memory_object_slab_create();
	if (!slab) {
		object_allocator_initialized = false;
		spinlock_unlock_irqrestore(&object_allocator_lock, state);
		return false;
	}
	object_slabs                 = slab;
	object_allocator_initialized = true;
	spinlock_unlock_irqrestore(&object_allocator_lock, state);
	return true;
}

static struct memory_object* memory_object_control_alloc(void) {
	struct memory_object_slab* slab;
	struct memory_object*      object;
	struct irq_state           state;

	state = spinlock_lock_irqsave(&object_allocator_lock);
	if (!object_allocator_initialized) {
		spinlock_unlock_irqrestore(&object_allocator_lock, state);
		return NULL;
	}
	for (slab = object_slabs; slab != NULL; slab = slab->next) {
		if (slab->free_slots != NULL) break;
	}
	if (!slab) {
		slab = memory_object_slab_create();
		if (!slab) {
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
	object->slab = slab;
	spinlock_unlock_irqrestore(&object_allocator_lock, state);
	return object;
}

static void memory_object_control_free(struct memory_object* object) {
	struct memory_object_free_slot* free_slot;
	struct memory_object_slab*      slab;
	struct memory_object_slab**     link;
	struct irq_state                state;

	if (!object) return;
	slab  = object->slab;
	state = spinlock_lock_irqsave(&object_allocator_lock);
	if (!slab || slab->used == 0u) hcf();
	memset(object, 0, sizeof(*object));
	free_slot        = (struct memory_object_free_slot*)object;
	free_slot->next  = slab->free_slots;
	slab->free_slots = free_slot;
	slab->used--;
	if (slab != object_slabs && slab->used == 0u) {
		link = &object_slabs;
		while (*link != NULL && *link != slab) link = &(*link)->next;
		if (*link != slab) hcf();
		*link = slab->next;
		(void)pmm_free_pages(slab->phys, 1u);
	}
	spinlock_unlock_irqrestore(&object_allocator_lock, state);
}

static bool memory_object_alloc(enum memory_object_type type, size_t page_count, struct memory_object** out_object) {
	struct memory_object*      object;
	struct memory_object_slab* slab;

	if (out_object) *out_object = NULL;
	if (!out_object || page_count == 0) return false;
	object = memory_object_control_alloc();
	if (!object) return false;
	slab    = object->slab;
	*object = (struct memory_object){
		.type            = type,
		.page_count      = page_count,
		.reference_count = 1u,
		.slab            = slab,
	};
	spinlock_init_class(&object->lock,
	                    "memory_object",
	                    SPINLOCK_ORDER_MEMORY_OBJECT,
	                    SPINLOCK_FLAG_IRQSAVE | SPINLOCK_FLAG_ALLOW_EXCEPTION);
	backing_store_init(&object->backing, page_count);
	*out_object = object;
	return true;
}

bool memory_object_create_anonymous(size_t page_count, struct memory_object** out_object) {
	return memory_object_alloc(MEMORY_OBJECT_ANONYMOUS, page_count, out_object);
}

bool memory_object_create_external(uintptr_t phys_base, size_t page_count, struct memory_object** out_object) {
	struct memory_object* object;
	uint64_t              last_page_offset;
	uint64_t              last_page_base;

	if (out_object) *out_object = NULL;
	if (!out_object || page_count == 0 || (phys_base & (PMM_PAGE_SIZE - 1u)) != 0) return false;
	if (mul_overflow_u64((uint64_t)(page_count - 1u), PMM_PAGE_SIZE, &last_page_offset) ||
	    add_overflow_u64((uint64_t)phys_base, last_page_offset, &last_page_base) ||
	    last_page_base > (uint64_t)UINTPTR_MAX - (PMM_PAGE_SIZE - 1u))
		return false;
	if (!memory_object_alloc(MEMORY_OBJECT_EXTERNAL_PHYSICAL, page_count, &object)) return false;
	if (!backing_store_ensure(&object->backing)) {
		memory_object_control_free(object);
		return false;
	}
	for (size_t page = 0; page < page_count; page++) {
		backing_store_set_entry(&object->backing, page, phys_base + page * (uintptr_t)PMM_PAGE_SIZE);
	}
	*out_object = object;
	return true;
}

bool memory_object_retain(struct memory_object* object) {
	uint64_t current;

	if (!object) return false;
	current = __atomic_load_n(&object->reference_count, __ATOMIC_ACQUIRE);
	for (;;) {
		if (current == 0u || current == UINT64_MAX) return false;
		if (__atomic_compare_exchange_n(
				&object->reference_count, &current, current + 1u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
			return true;
	}
}

void memory_object_release(struct memory_object* object) {
	uint64_t old_reference_count;

	if (!object) return;
	old_reference_count = __atomic_fetch_sub(&object->reference_count, 1u, __ATOMIC_ACQ_REL);
	if (old_reference_count == 0u) hcf();
	if (old_reference_count != 1u) return;
	if (object->type == MEMORY_OBJECT_ANONYMOUS) backing_store_release(&object->backing);
	else backing_store_release_metadata(&object->backing);
	memory_object_control_free(object);
}

enum memory_object_type memory_object_type(const struct memory_object* object) {
	return object != NULL ? object->type : MEMORY_OBJECT_ANONYMOUS;
}

size_t memory_object_page_count(const struct memory_object* object) {
	return object != NULL ? object->page_count : 0u;
}

bool memory_object_page_phys(struct memory_object* object, size_t logical_page, uintptr_t* out_phys) {
	struct irq_state state;
	uintptr_t        phys;

	if (out_phys) *out_phys = 0u;
	if (!object || !out_phys || logical_page >= object->page_count) return false;
	state = spinlock_lock_irqsave(&object->lock);
	phys  = backing_page_phys(backing_store_entry(&object->backing, logical_page));
	spinlock_unlock_irqrestore(&object->lock, state);
	if (phys == 0u) return false;
	*out_phys = phys;
	return true;
}

bool memory_object_ensure_page(struct memory_object* object, size_t logical_page, uintptr_t* out_phys,
                               bool* out_allocated) {
	struct irq_state state;
	bool             allocated = false;
	bool             ok;

	if (out_phys) *out_phys = 0u;
	if (out_allocated) *out_allocated = false;
	if (!object || !out_phys || logical_page >= object->page_count) return false;
	state = spinlock_lock_irqsave(&object->lock);
	ok    = backing_store_ensure_page(&object->backing, logical_page, out_phys, &allocated);
	if (ok && allocated && object->type == MEMORY_OBJECT_ANONYMOUS)
		memset(hhdm_phys_to_virt(*out_phys), 0, PMM_PAGE_SIZE);
	spinlock_unlock_irqrestore(&object->lock, state);
	if (ok && out_allocated) *out_allocated = allocated;
	return ok;
}

bool memory_object_release_page(struct memory_object* object, size_t logical_page) {
	struct irq_state state;
	uintptr_t        phys;
	bool             ok;

	if (!object || logical_page >= object->page_count) return false;
	state = spinlock_lock_irqsave(&object->lock);
	if (object->type != MEMORY_OBJECT_ANONYMOUS) {
		spinlock_unlock_irqrestore(&object->lock, state);
		return true;
	}
	phys = backing_page_phys(backing_store_entry(&object->backing, logical_page));
	if (phys == 0u) {
		spinlock_unlock_irqrestore(&object->lock, state);
		return true;
	}
	ok = pmm_free_pages(phys, 1u);
	if (ok) {
		backing_store_set_entry(&object->backing, logical_page, 0u);
		backing_store_release_if_empty(&object->backing);
	}
	spinlock_unlock_irqrestore(&object->lock, state);
	return ok;
}

bool memory_object_release_pages(struct memory_object* object, size_t first_page, size_t page_count) {
	size_t end_page;

	if (!object || page_count == 0 || add_overflow_size(first_page, page_count, &end_page) ||
	    end_page > object->page_count)
		return false;
	if (object->type == MEMORY_OBJECT_EXTERNAL_PHYSICAL) return true;
	for (size_t page = first_page; page < end_page; page++) {
		if (!memory_object_release_page(object, page)) return false;
	}
	return true;
}
