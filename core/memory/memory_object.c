#include <base/math.h>
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

static struct object_slab* object_slabs;
static struct spinlock     object_allocator_lock =
	SPINLOCK_INIT_CLASS("memory_object_allocator", SPINLOCK_ORDER_MEMORY_OBJECT, SPINLOCK_FLAG_IRQSAVE);

#define OBJECT_ALIGN _Alignof(struct memory_object)
#define OBJECT_SLOT_SIZE ((sizeof(struct memory_object) + OBJECT_ALIGN - 1u) & ~(OBJECT_ALIGN - 1u))
#define OBJECT_SLAB_OFFSET ((sizeof(struct object_slab) + OBJECT_ALIGN - 1u) & ~(OBJECT_ALIGN - 1u))

_Static_assert(OBJECT_SLAB_OFFSET + OBJECT_SLOT_SIZE <= PMM_PAGE_SIZE, "object slab must contain an object");

static void* phys_to_virt(uintptr_t phys) {
	return (void*)(uintptr_t)(phys + boot_info.direct_map_offset);
}

static struct object_slab* slab_create(void) {
	uintptr_t           phys = 0u;
	struct object_slab* slab;
	uint8_t*            slots;
	size_t              count;
	if (!pmm_alloc_pages(1u, &phys)) return NULL;
	slab = phys_to_virt(phys);
	memset(slab, 0, PMM_PAGE_SIZE);
	slab->phys = phys;
	slots      = (uint8_t*)slab + OBJECT_SLAB_OFFSET;
	count      = (PMM_PAGE_SIZE - OBJECT_SLAB_OFFSET) / OBJECT_SLOT_SIZE;
	for (size_t i = 0u; i < count; i++) {
		struct object_free_slot* slot = (void*)(slots + i * OBJECT_SLOT_SIZE);
		slot->next                    = slab->free_slots;
		slab->free_slots              = slot;
	}
	return slab;
}

static struct object_slab* object_slab(const struct memory_object* object) {
	return (struct object_slab*)((uintptr_t)object & ~(uintptr_t)(PMM_PAGE_SIZE - 1u));
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
		(void)pmm_free_pages(slab->phys, 1u);
	}
	spinlock_unlock_irqrestore(&object_allocator_lock, state);
}

static bool object_create(enum memory_object_type type, size_t page_count, struct memory_object** out_object) {
	struct memory_object* object;
	if (out_object != NULL) *out_object = NULL;
	if (out_object == NULL || page_count == 0u || page_count > SIZE_MAX / PMM_PAGE_SIZE) return false;
	object = control_alloc();
	if (object == NULL) return false;
	object->page_count      = page_count;
	object->reference_count = 1u;
	object->type            = (uint8_t)type;
	object->radix_depth     = type == MEMORY_OBJECT_OWNED ? memory_object_radix_depth(page_count) : 0u;
	spinlock_init_class(&object->lock,
	                    "memory_object",
	                    SPINLOCK_ORDER_MEMORY_OBJECT,
	                    SPINLOCK_FLAG_IRQSAVE | SPINLOCK_FLAG_ALLOW_EXCEPTION);
	*out_object = object;
	return true;
}

bool memory_object_create_owned(size_t page_count, struct memory_object** out_object) {
	return object_create(MEMORY_OBJECT_OWNED, page_count, out_object);
}

bool memory_object_create_external(uintptr_t phys_base, size_t page_count, struct memory_object** out_object) {
	struct memory_object* object;
	uint64_t              span;
	if (out_object != NULL) *out_object = NULL;
	if ((phys_base & (PMM_PAGE_SIZE - 1u)) != 0u || page_count == 0u ||
	    mul_overflow_u64((uint64_t)page_count, PMM_PAGE_SIZE, &span) || span - 1u > UINTPTR_MAX - phys_base)
		return false;
	if (!object_create(MEMORY_OBJECT_EXTERNAL, page_count, &object)) return false;
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
	control_free(object);
}

enum memory_object_type memory_object_type(const struct memory_object* object) {
	return object == NULL ? MEMORY_OBJECT_OWNED : (enum memory_object_type)object->type;
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
	if (object->type == MEMORY_OBJECT_EXTERNAL) {
		*out_phys = object->backing_root_or_phys + logical_page * (uintptr_t)PMM_PAGE_SIZE;
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
	if (object->type == MEMORY_OBJECT_EXTERNAL) {
		*out_phys = object->backing_root_or_phys + logical_page * (uintptr_t)PMM_PAGE_SIZE;
		ok        = true;
	}
	else ok = memory_object_radix_resolve(object, logical_page, out_phys);
	spinlock_unlock_irqrestore(&object->lock, state);
	return ok;
}

static bool access_bounds(const struct memory_object* object, size_t offset, size_t size) {
	size_t bytes;
	return object != NULL && !mul_overflow_size(object->page_count, PMM_PAGE_SIZE, &bytes) && offset <= bytes &&
	       size <= bytes - offset;
}

bool memory_object_read(struct memory_object* object, size_t byte_offset, void* dst, size_t size) {
	struct irq_state state;
	size_t           done = 0u;
	if (size == 0u) return access_bounds(object, byte_offset, 0u);
	if (dst == NULL || !access_bounds(object, byte_offset, size)) return false;
	state = spinlock_lock_irqsave(&object->lock);
	while (done < size) {
		size_t    offset = byte_offset + done, page = offset / PMM_PAGE_SIZE;
		size_t    within = offset & (PMM_PAGE_SIZE - 1u), chunk = PMM_PAGE_SIZE - within;
		uintptr_t phys;
		if (chunk > size - done) chunk = size - done;
		if (object->type == MEMORY_OBJECT_EXTERNAL) phys = object->backing_root_or_phys + page * PMM_PAGE_SIZE;
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
		size_t    offset = byte_offset + done, page = offset / PMM_PAGE_SIZE;
		size_t    within = offset & (PMM_PAGE_SIZE - 1u), chunk = PMM_PAGE_SIZE - within;
		uintptr_t phys;
		if (chunk > size - done) chunk = size - done;
		if (object->type == MEMORY_OBJECT_EXTERNAL) phys = object->backing_root_or_phys + page * PMM_PAGE_SIZE;
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
