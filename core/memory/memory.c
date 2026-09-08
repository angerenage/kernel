#include <base/math.h>
#include <core/memory.h>
#include <core/memory_backing.h>
#include <core/mm.h>
#include <core/pmm.h>
#include <core/spinlock.h>
#include <hal/hcf.h>
#include <stddef.h>
#include <string.h>

struct memory {
	struct memory_backing* backing;
	struct memory*         parent;
	size_t                 backing_offset;
	size_t                 size;
	uint64_t               reference_count;
	enum memory_type       memory_type;
};

struct memory_free_slot {
	struct memory_free_slot* next;
};

struct memory_slab {
	struct memory_slab*      next;
	struct memory_free_slot* free_slots;
	uintptr_t                physical_address;
	size_t                   used;
};

static struct memory_slab* memory_slabs;
static struct spinlock     memory_allocator_lock =
	SPINLOCK_INIT_CLASS("memory_allocator", SPINLOCK_ORDER_MEMORY, SPINLOCK_FLAG_IRQSAVE);

#define MEMORY_ALIGN _Alignof(struct memory)
#define MEMORY_SLOT_SIZE ((sizeof(struct memory) + MEMORY_ALIGN - 1u) & ~(MEMORY_ALIGN - 1u))
#define MEMORY_SLAB_OFFSET ((sizeof(struct memory_slab) + MEMORY_ALIGN - 1u) & ~(MEMORY_ALIGN - 1u))

_Static_assert(sizeof(struct memory) >= sizeof(struct memory_free_slot), "Memory slot must hold a free-list link");

static void* physical_to_virtual(uintptr_t physical_address) {
	return (void*)(physical_address + boot_info.direct_map_offset);
}

static size_t memory_granule(void) {
	const struct pmm_info* info = pmm_info();
	return info == NULL ? 0u : info->allocation_granule;
}

static struct memory_slab* slab_create(size_t granule) {
	struct pmm_extent   allocation;
	struct memory_slab* slab;
	uint8_t*            slots;
	size_t              count;
	if (granule <= MEMORY_SLAB_OFFSET || !pmm_alloc(&(const struct pmm_alloc_request){.size = granule}, &allocation))
		return NULL;
	slab = physical_to_virtual(allocation.address);
	memset(slab, 0, granule);
	count = (granule - MEMORY_SLAB_OFFSET) / MEMORY_SLOT_SIZE;
	if (count == 0u) {
		(void)pmm_free(allocation);
		return NULL;
	}
	slab->physical_address = allocation.address;
	slots                  = (uint8_t*)slab + MEMORY_SLAB_OFFSET;
	for (size_t i = 0u; i < count; i++) {
		struct memory_free_slot* slot = (void*)(slots + i * MEMORY_SLOT_SIZE);
		slot->next                    = slab->free_slots;
		slab->free_slots              = slot;
	}
	return slab;
}

static struct memory_slab* memory_slab(const struct memory* memory, size_t granule) {
	uintptr_t physical = (uintptr_t)memory - boot_info.direct_map_offset;
	physical &= ~(uintptr_t)(granule - 1u);
	return physical_to_virtual(physical);
}

static struct memory* control_alloc(void) {
	struct irq_state    state = spinlock_lock_irqsave(&memory_allocator_lock);
	struct memory_slab* slab;
	struct memory*      memory;
	size_t              granule = memory_granule();
	if (granule == 0u) {
		spinlock_unlock_irqrestore(&memory_allocator_lock, state);
		return NULL;
	}
	for (slab = memory_slabs; slab != NULL && slab->free_slots == NULL; slab = slab->next) {
	}
	if (slab == NULL) {
		slab = slab_create(granule);
		if (slab == NULL) {
			spinlock_unlock_irqrestore(&memory_allocator_lock, state);
			return NULL;
		}
		slab->next   = memory_slabs;
		memory_slabs = slab;
	}
	memory           = (struct memory*)slab->free_slots;
	slab->free_slots = slab->free_slots->next;
	slab->used++;
	memset(memory, 0, sizeof(*memory));
	spinlock_unlock_irqrestore(&memory_allocator_lock, state);
	return memory;
}

static void control_free(struct memory* memory) {
	struct memory_slab*      slab;
	struct memory_slab**     link;
	struct memory_free_slot* slot;
	struct irq_state         state;
	uintptr_t                physical_address = 0u;
	size_t                   granule          = memory_granule();
	bool                     release          = false;
	if (memory == NULL || granule == 0u) hcf();
	slab  = memory_slab(memory, granule);
	state = spinlock_lock_irqsave(&memory_allocator_lock);
	if (slab->used == 0u) hcf();
	memset(memory, 0, sizeof(*memory));
	slot             = (struct memory_free_slot*)memory;
	slot->next       = slab->free_slots;
	slab->free_slots = slot;
	slab->used--;
	if (slab->used == 0u) {
		for (link = &memory_slabs; *link != NULL && *link != slab; link = &(*link)->next) {
		}
		if (*link != slab) hcf();
		*link            = slab->next;
		physical_address = slab->physical_address;
		release          = true;
	}
	spinlock_unlock_irqrestore(&memory_allocator_lock, state);
	if (release && !pmm_free((struct pmm_extent){.address = physical_address, .size = granule})) hcf();
}

static bool memory_type_valid(enum memory_type type) {
	return type >= MEMORY_TYPE_NORMAL && type < MEMORY_TYPE_COUNT;
}

static bool range_valid(const struct memory* memory, size_t offset, size_t size) {
	return memory != NULL && offset <= memory->size && size <= memory->size - offset;
}

static bool backing_offset(const struct memory* memory, size_t offset, size_t* out_offset) {
	return memory != NULL && out_offset != NULL && !add_overflow_size(memory->backing_offset, offset, out_offset);
}

static bool create_root(struct memory_backing* backing, size_t size, enum memory_type type,
                        struct memory** out_memory) {
	struct memory* memory = control_alloc();
	if (memory == NULL) return false;
	memory->backing         = backing;
	memory->size            = size;
	memory->reference_count = 1u;
	memory->memory_type     = type;
	*out_memory             = memory;
	return true;
}

bool memory_create_anonymous(size_t size, struct memory** out_memory) {
	struct memory_backing* backing;
	size_t                 capacity;
	size_t                 granule = memory_granule();
	if (out_memory != NULL) *out_memory = NULL;
	if (out_memory == NULL || size == 0u || granule == 0u || !align_up_size(size, granule, &capacity) ||
	    !memory_backing_create_anonymous(capacity, &backing))
		return false;
	if (!create_root(backing, size, MEMORY_TYPE_NORMAL, out_memory)) {
		memory_backing_release(backing);
		return false;
	}
	return true;
}

bool memory_create_physical(const struct memory_physical_request* request, struct memory** out_memory) {
	struct memory_backing* backing;
	if (out_memory != NULL) *out_memory = NULL;
	if (out_memory == NULL || request == NULL || !memory_type_valid(request->memory_type) ||
	    !memory_backing_create_physical(
			&(const struct memory_backing_physical_request){
				.physical_address        = request->physical_address,
				.size                    = request->size,
				.external_cpu_accessible = request->external_cpu_accessible,
			},
			&backing))
		return false;
	if (!create_root(backing, request->size, request->memory_type, out_memory)) {
		memory_backing_release(backing);
		return false;
	}
	return true;
}

bool memory_slice(struct memory* parent, size_t offset, size_t size, struct memory** out_memory) {
	struct memory* child;
	size_t         absolute_offset;
	if (out_memory != NULL) *out_memory = NULL;
	if (out_memory == NULL || size == 0u || !range_valid(parent, offset, size) ||
	    !backing_offset(parent, offset, &absolute_offset))
		return false;
	child = control_alloc();
	if (child == NULL) return false;
	if (!memory_retain(parent)) {
		control_free(child);
		return false;
	}
	child->backing         = parent->backing;
	child->parent          = parent;
	child->backing_offset  = absolute_offset;
	child->size            = size;
	child->reference_count = 1u;
	child->memory_type     = parent->memory_type;
	*out_memory            = child;
	return true;
}

bool memory_retain(struct memory* memory) {
	uint64_t current;
	if (memory == NULL) return false;
	current = __atomic_load_n(&memory->reference_count, __ATOMIC_ACQUIRE);
	for (;;) {
		if (current == 0u || current == UINT64_MAX) return false;
		if (__atomic_compare_exchange_n(
				&memory->reference_count, &current, current + 1u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
			return true;
	}
}

void memory_release(struct memory* memory) {
	while (memory != NULL) {
		uint64_t               old     = __atomic_fetch_sub(&memory->reference_count, 1u, __ATOMIC_ACQ_REL);
		struct memory*         parent  = memory->parent;
		struct memory_backing* backing = memory->backing;
		if (old == 0u) hcf();
		if (old != 1u) return;
		control_free(memory);
		if (parent == NULL) {
			memory_backing_release(backing);
			return;
		}
		memory = parent;
	}
}

size_t memory_size(const struct memory* memory) {
	return memory == NULL ? 0u : memory->size;
}

enum memory_type memory_type(const struct memory* memory) {
	return memory == NULL ? MEMORY_TYPE_NORMAL : memory->memory_type;
}

bool memory_can_transfer(const struct memory* memory) {
	return memory != NULL && memory->memory_type == MEMORY_TYPE_NORMAL && memory_cpu_accessible(memory);
}

bool memory_cpu_accessible(const struct memory* memory) {
	return memory != NULL && memory_backing_cpu_accessible(memory->backing);
}

bool memory_range_is_aligned(const struct memory* memory, size_t offset, size_t size, size_t alignment) {
	size_t absolute_offset;
	return alignment != 0u && (alignment & (alignment - 1u)) == 0u && range_valid(memory, offset, size) &&
	       backing_offset(memory, offset, &absolute_offset) && (absolute_offset & (alignment - 1u)) == 0u &&
	       (size & (alignment - 1u)) == 0u;
}

bool memory_query(struct memory* memory, size_t offset, size_t maximum_size, struct memory_span* out_span) {
	struct memory_backing_span backing_span;
	size_t                     absolute_offset;
	if (out_span != NULL) *out_span = (struct memory_span){0};
	if (out_span == NULL || memory == NULL || maximum_size == 0u || offset >= memory->size ||
	    !backing_offset(memory, offset, &absolute_offset))
		return false;
	if (maximum_size > memory->size - offset) maximum_size = memory->size - offset;
	if (!memory_backing_query(memory->backing, absolute_offset, maximum_size, &backing_span)) return false;
	*out_span = (struct memory_span){
		.kind             = backing_span.kind == MEMORY_BACKING_SPAN_PRESENT ? MEMORY_SPAN_PRESENT : MEMORY_SPAN_HOLE,
		.offset           = offset,
		.size             = backing_span.size,
		.physical_address = backing_span.physical_address,
	};
	return true;
}

bool memory_materialize(struct memory* memory, const struct memory_materialize_request* request) {
	struct memory_backing_span span;
	size_t                     absolute_offset;
	size_t                     granule = memory_granule();
	size_t                     alignment;
	if (memory == NULL || request == NULL || request->size == 0u || granule == 0u ||
	    !range_valid(memory, request->offset, request->size) ||
	    !backing_offset(memory, request->offset, &absolute_offset))
		return false;
	alignment = request->alignment == 0u ? granule : request->alignment;
	if ((absolute_offset & (granule - 1u)) != 0u || (request->size & (granule - 1u)) != 0u || alignment < granule ||
	    (alignment & (alignment - 1u)) != 0u ||
	    (request->maximum_address != 0u && request->maximum_address <= request->minimum_address))
		return false;
	if (memory_backing_kind(memory->backing) == MEMORY_BACKING_PHYSICAL) {
		uintptr_t contiguous_base = 0u;
		for (size_t done = 0u; done < request->size; done += span.size) {
			if (!memory_backing_query(memory->backing, absolute_offset + done, request->size - done, &span) ||
			    span.kind != MEMORY_BACKING_SPAN_PRESENT)
				return false;
			if (request->require_contiguous) {
				if (done == 0u) contiguous_base = span.physical_address;
				else if (done > UINTPTR_MAX - contiguous_base || span.physical_address != contiguous_base + done)
					return false;
			}
		}
		return true;
	}
	return memory_backing_materialize(memory->backing,
	                                  &(const struct memory_backing_materialize_request){
										  .offset             = absolute_offset,
										  .size               = request->size,
										  .alignment          = request->alignment,
										  .minimum_address    = request->minimum_address,
										  .maximum_address    = request->maximum_address,
										  .require_contiguous = request->require_contiguous,
									  });
}

bool memory_read(struct memory* memory, size_t offset, void* destination, size_t size) {
	size_t absolute_offset;
	if (!memory_can_transfer(memory)) return false;
	if (size == 0u) return range_valid(memory, offset, 0u);
	return destination != NULL && range_valid(memory, offset, size) &&
	       backing_offset(memory, offset, &absolute_offset) &&
	       memory_backing_read(memory->backing, absolute_offset, destination, size);
}

bool memory_write(struct memory* memory, size_t offset, const void* source, size_t size) {
	size_t absolute_offset;
	if (!memory_can_transfer(memory)) return false;
	if (size == 0u) return range_valid(memory, offset, 0u);
	return source != NULL && range_valid(memory, offset, size) && backing_offset(memory, offset, &absolute_offset) &&
	       memory_backing_write(memory->backing, absolute_offset, source, size);
}
