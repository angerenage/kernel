#include <base/math.h>
#include <base/vmm.h>
#include <core/memory_object.h>
#include <core/mm.h>
#include <core/pmm.h>
#include <core/spinlock.h>
#include <hal/hcf.h>
#include <stddef.h>
#include <string.h>

struct object_free_slot {
	struct object_free_slot* next;
};

struct object_slab {
	struct object_slab*      next;
	struct object_free_slot* free_slots;
	uintptr_t                physical_address;
	size_t                   used;
};

static struct object_slab* object_slabs;
static struct spinlock     object_allocator_lock =
	SPINLOCK_INIT_CLASS("memory_object_allocator", SPINLOCK_ORDER_MEMORY_OBJECT, SPINLOCK_FLAG_IRQSAVE);

#define OBJECT_ALIGN _Alignof(struct memory_object)
#define OBJECT_SLOT_SIZE ((sizeof(struct memory_object) + OBJECT_ALIGN - 1u) & ~(OBJECT_ALIGN - 1u))
#define OBJECT_SLAB_OFFSET ((sizeof(struct object_slab) + OBJECT_ALIGN - 1u) & ~(OBJECT_ALIGN - 1u))

_Static_assert(sizeof(struct memory_object) >= sizeof(struct object_free_slot),
               "object slot must hold a free-list link");

static void* physical_to_virtual(uintptr_t physical_address) {
	return (void*)(physical_address + boot_info.direct_map_offset);
}

static size_t object_granule(void) {
	const struct pmm_info* info = pmm_info();
	return info == NULL ? 0u : info->allocation_granule;
}

static struct object_slab* slab_create(size_t granule) {
	struct pmm_extent   allocation;
	struct object_slab* slab;
	uint8_t*            slots;
	size_t              count;
	if (granule <= OBJECT_SLAB_OFFSET || !pmm_alloc(&(const struct pmm_alloc_request){.size = granule}, &allocation))
		return NULL;
	slab = physical_to_virtual(allocation.address);
	memset(slab, 0, granule);
	count = (granule - OBJECT_SLAB_OFFSET) / OBJECT_SLOT_SIZE;
	if (count == 0u) {
		(void)pmm_free(allocation);
		return NULL;
	}
	slab->physical_address = allocation.address;
	slots                  = (uint8_t*)slab + OBJECT_SLAB_OFFSET;
	for (size_t i = 0u; i < count; i++) {
		struct object_free_slot* slot = (void*)(slots + i * OBJECT_SLOT_SIZE);
		slot->next                    = slab->free_slots;
		slab->free_slots              = slot;
	}
	return slab;
}

static struct object_slab* object_slab(const struct memory_object* object, size_t granule) {
	uintptr_t physical = (uintptr_t)object - boot_info.direct_map_offset;
	physical &= ~(uintptr_t)(granule - 1u);
	return physical_to_virtual(physical);
}

static struct memory_object* control_alloc(void) {
	struct irq_state      state = spinlock_lock_irqsave(&object_allocator_lock);
	struct object_slab*   slab;
	struct memory_object* object;
	size_t                granule = object_granule();
	if (granule == 0u) {
		spinlock_unlock_irqrestore(&object_allocator_lock, state);
		return NULL;
	}
	for (slab = object_slabs; slab != NULL && slab->free_slots == NULL; slab = slab->next) {
	}
	if (slab == NULL) {
		slab = slab_create(granule);
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
	struct object_slab*      slab;
	struct object_slab**     link;
	struct object_free_slot* slot;
	struct irq_state         state;
	uintptr_t                physical_address = 0u;
	size_t                   granule          = object_granule();
	bool                     release          = false;
	if (object == NULL || granule == 0u) hcf();
	slab  = object_slab(object, granule);
	state = spinlock_lock_irqsave(&object_allocator_lock);
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
		*link            = slab->next;
		physical_address = slab->physical_address;
		release          = true;
	}
	spinlock_unlock_irqrestore(&object_allocator_lock, state);
	if (release && !pmm_free((struct pmm_extent){.address = physical_address, .size = granule})) hcf();
}

static bool object_span(size_t page_count, size_t* out_span) {
	return page_count != 0u && !mul_overflow_size(page_count, VMM_PAGE_SIZE, out_span);
}

bool memory_object_create_params_valid(const struct memory_create_params* params) {
	const struct memory_constraints* constraints;
	size_t                           granule = object_granule();
	size_t                           align_pages;
	size_t                           align_bytes;
	size_t                           span;
	uint64_t                         fixed_end;
	if (params == NULL || granule == 0u || (granule & (granule - 1u)) != 0u || VMM_PAGE_SIZE % granule != 0u ||
	    !object_span(params->page_count, &span) || params->memory_type >= MEMORY_TYPE_COUNT)
		return false;
	constraints = &params->constraints;
	if (params->memory_type != MEMORY_TYPE_NORMAL && (constraints->flags & MEMORY_CONSTRAINT_FIXED) == 0u) return false;
	if ((constraints->flags & ~(uint32_t)(MEMORY_CONSTRAINT_CONTIGUOUS | MEMORY_CONSTRAINT_FIXED)) != 0u ||
	    (constraints->physical_min & (granule - 1u)) != 0u ||
	    (constraints->physical_max != 0u && ((constraints->physical_max & (granule - 1u)) != 0u ||
	                                         constraints->physical_max <= constraints->physical_min)))
		return false;
	align_pages = constraints->align_pages == 0u ? 1u : constraints->align_pages;
	if ((align_pages & (align_pages - 1u)) != 0u || mul_overflow_size(align_pages, VMM_PAGE_SIZE, &align_bytes))
		return false;
	if ((constraints->flags & MEMORY_CONSTRAINT_FIXED) == 0u) {
		if (constraints->physical_address != 0u ||
		    (align_pages > 1u && (constraints->flags & MEMORY_CONSTRAINT_CONTIGUOUS) == 0u))
			return false;
		if ((constraints->flags & MEMORY_CONSTRAINT_CONTIGUOUS) != 0u && constraints->physical_max != 0u &&
		    span > constraints->physical_max - constraints->physical_min)
			return false;
		return true;
	}
	if ((constraints->physical_address & (granule - 1u)) != 0u ||
	    (constraints->physical_address & (align_bytes - 1u)) != 0u ||
	    constraints->physical_address < constraints->physical_min ||
	    add_overflow_u64((uint64_t)constraints->physical_address, span, &fixed_end) || fixed_end > UINTPTR_MAX ||
	    (constraints->physical_max != 0u && fixed_end > constraints->physical_max))
		return false;
	return true;
}

static bool object_create(enum memory_type memory_type, size_t page_count, struct memory_backing* backing,
                          struct memory_object** out_object) {
	struct memory_object* object = control_alloc();
	if (object == NULL) return false;
	object->backing         = backing;
	object->page_count      = page_count;
	object->reference_count = 1u;
	object->memory_type     = (uint8_t)memory_type;
	*out_object             = object;
	return true;
}

static bool materialize_legacy_pages(struct memory_backing* backing, size_t page_count,
                                     const struct memory_constraints* constraints, bool contiguous) {
	if (contiguous) {
		return memory_backing_materialize(backing,
		                                  &(const struct memory_backing_materialize_request){
											  .size               = page_count * VMM_PAGE_SIZE,
											  .alignment          = constraints->align_pages * VMM_PAGE_SIZE,
											  .minimum_address    = constraints->physical_min,
											  .maximum_address    = constraints->physical_max,
											  .require_contiguous = true,
										  });
	}
	for (size_t page = 0u; page < page_count; page++) {
		if (!memory_backing_materialize(backing,
		                                &(const struct memory_backing_materialize_request){
											.offset             = page * VMM_PAGE_SIZE,
											.size               = VMM_PAGE_SIZE,
											.minimum_address    = constraints->physical_min,
											.maximum_address    = constraints->physical_max,
											.require_contiguous = true,
										}))
			return false;
	}
	return true;
}

static bool zero_physical_backing(struct memory_backing* backing, size_t size) {
	struct memory_backing_span span;
	for (size_t offset = 0u; offset < size; offset += span.size) {
		if (!memory_backing_query(backing, offset, size - offset, &span) || span.kind != MEMORY_BACKING_SPAN_PRESENT)
			return false;
		memset(physical_to_virtual(span.physical_address), 0, span.size);
	}
	return true;
}

bool memory_object_create(const struct memory_create_params* params, struct memory_object** out_object) {
	const struct memory_constraints* constraints;
	struct memory_backing*           backing;
	size_t                           span;
	bool                             contiguous;
	if (out_object != NULL) *out_object = NULL;
	if (out_object == NULL || !memory_object_create_params_valid(params) || !object_span(params->page_count, &span))
		return false;
	constraints = &params->constraints;
	contiguous  = (constraints->flags & MEMORY_CONSTRAINT_CONTIGUOUS) != 0u;
	if ((constraints->flags & MEMORY_CONSTRAINT_FIXED) != 0u) {
		if (!memory_backing_create_physical(
				&(const struct memory_backing_physical_request){
					.physical_address = constraints->physical_address,
					.size             = span,
				},
				&backing))
			return false;
		if (memory_backing_cpu_accessible(backing) &&
		    (params->memory_type != MEMORY_TYPE_NORMAL || !zero_physical_backing(backing, span))) {
			memory_backing_release(backing);
			return false;
		}
	}
	else {
		if (!memory_backing_create_anonymous(span, &backing)) return false;
		if ((contiguous || constraints->physical_min != 0u || constraints->physical_max != 0u) &&
		    !materialize_legacy_pages(backing, params->page_count, constraints, contiguous)) {
			memory_backing_release(backing);
			return false;
		}
	}
	if (!object_create(params->memory_type, params->page_count, backing, out_object)) {
		memory_backing_release(backing);
		return false;
	}
	return true;
}

bool memory_object_create_owned(size_t page_count, struct memory_object** out_object) {
	return memory_object_create(
		&(const struct memory_create_params){.page_count = page_count, .memory_type = MEMORY_TYPE_NORMAL}, out_object);
}

bool memory_object_create_external(uintptr_t phys_base, size_t page_count, struct memory_object** out_object) {
	struct memory_backing* backing;
	struct memory_object*  object;
	size_t                 span;
	if (out_object != NULL) *out_object = NULL;
	if (out_object == NULL || !object_span(page_count, &span) || object_granule() == 0u ||
	    (phys_base & (object_granule() - 1u)) != 0u || span > UINTPTR_MAX - phys_base ||
	    !memory_backing_create_physical(
			&(const struct memory_backing_physical_request){
				.physical_address        = phys_base,
				.size                    = span,
				.external_cpu_accessible = true,
			},
			&backing))
		return false;
	if (!object_create(MEMORY_TYPE_NORMAL, page_count, backing, &object)) {
		memory_backing_release(backing);
		return false;
	}
	*out_object = object;
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
	memory_backing_release(object->backing);
	control_free(object);
}

enum memory_type memory_object_memory_type(const struct memory_object* object) {
	return object == NULL ? MEMORY_TYPE_NORMAL : (enum memory_type)object->memory_type;
}

bool memory_object_can_transfer(const struct memory_object* object) {
	return object != NULL && memory_object_memory_type(object) == MEMORY_TYPE_NORMAL &&
	       memory_backing_cpu_accessible(object->backing);
}

size_t memory_object_page_count(const struct memory_object* object) {
	return object == NULL ? 0u : object->page_count;
}

bool memory_object_page_phys(struct memory_object* object, size_t logical_page, uintptr_t* out_phys) {
	struct memory_backing_span span;
	if (out_phys != NULL) *out_phys = 0u;
	if (object == NULL || out_phys == NULL || logical_page >= object->page_count ||
	    !memory_backing_query(object->backing, logical_page * VMM_PAGE_SIZE, VMM_PAGE_SIZE, &span) ||
	    span.kind != MEMORY_BACKING_SPAN_PRESENT || span.size != VMM_PAGE_SIZE)
		return false;
	*out_phys = span.physical_address;
	return true;
}

bool memory_object_resolve_page(struct memory_object* object, size_t logical_page, uintptr_t* out_phys) {
	size_t offset;
	if (out_phys != NULL) *out_phys = 0u;
	if (object == NULL || out_phys == NULL || logical_page >= object->page_count) return false;
	offset = logical_page * VMM_PAGE_SIZE;
	if (memory_backing_kind(object->backing) == MEMORY_BACKING_ANONYMOUS &&
	    !memory_backing_materialize(object->backing,
	                                &(const struct memory_backing_materialize_request){
										.offset             = offset,
										.size               = VMM_PAGE_SIZE,
										.require_contiguous = true,
									}))
		return false;
	return memory_object_page_phys(object, logical_page, out_phys);
}

static bool access_bounds(const struct memory_object* object, size_t offset, size_t size) {
	size_t bytes;
	return object != NULL && object_span(object->page_count, &bytes) && offset <= bytes && size <= bytes - offset;
}

bool memory_object_read(struct memory_object* object, size_t byte_offset, void* dst, size_t size) {
	if (size == 0u) return access_bounds(object, byte_offset, 0u);
	if (!memory_object_can_transfer(object) || !access_bounds(object, byte_offset, size)) return false;
	return memory_backing_read(object->backing, byte_offset, dst, size);
}

bool memory_object_write(struct memory_object* object, size_t byte_offset, const void* src, size_t size) {
	size_t    first_page;
	size_t    last_page;
	uintptr_t physical;
	if (size == 0u) return access_bounds(object, byte_offset, 0u);
	if (!memory_object_can_transfer(object) || !access_bounds(object, byte_offset, size)) return false;
	if (memory_backing_kind(object->backing) == MEMORY_BACKING_ANONYMOUS) {
		first_page = byte_offset / VMM_PAGE_SIZE;
		last_page  = (byte_offset + size - 1u) / VMM_PAGE_SIZE;
		for (size_t page = first_page; page <= last_page; page++) {
			if (!memory_object_resolve_page(object, page, &physical)) return false;
		}
	}
	return memory_backing_write(object->backing, byte_offset, src, size);
}
