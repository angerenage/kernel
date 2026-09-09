#include <base/math.h>
#include <base/process.h>
#include <core/mm.h>
#include <core/pmm.h>
#include <core/process.h>
#include <core/sched.h>
#include <core/thread.h>
#include <hal/cache.h>
#include <hal/hcf.h>
#include <string.h>

#include "address_space_internal.h"

struct mapping_free_slot {
	struct mapping_free_slot* next;
};

struct mapping_slab {
	struct mapping_slab*      next;
	struct mapping_free_slot* free_slots;
	uintptr_t                 physical_address;
	size_t                    used;
};

static struct address_space kernel_space = {
	.lock = SPINLOCK_INIT_CLASS("kernel_address_space", SPINLOCK_ORDER_VADDR,
                                SPINLOCK_FLAG_IRQSAVE | SPINLOCK_FLAG_ALLOW_EXCEPTION),
};
static struct mapping_slab* mapping_slabs;
static struct spinlock      mapping_allocator_lock =
	SPINLOCK_INIT_CLASS("mapping_allocator", SPINLOCK_ORDER_MEMORY, SPINLOCK_FLAG_IRQSAVE);
static bool initialized;

#define MAPPING_ALIGN _Alignof(struct mapping)
#define MAPPING_SLOT_SIZE ((sizeof(struct mapping) + MAPPING_ALIGN - 1u) & ~(MAPPING_ALIGN - 1u))
#define MAPPING_SLAB_OFFSET ((sizeof(struct mapping_slab) + MAPPING_ALIGN - 1u) & ~(MAPPING_ALIGN - 1u))

_Static_assert(sizeof(struct mapping) >= sizeof(struct mapping_free_slot), "Mapping slot must hold a free-list link");

static void* physical_to_virtual(uintptr_t physical_address) {
	return (void*)(physical_address + boot_info.direct_map_offset);
}

static size_t mapping_granule(void) {
	const struct pmm_info* info = pmm_info();
	return info == NULL ? 0u : info->allocation_granule;
}

static struct mapping_slab* slab_create(size_t granule) {
	struct pmm_extent    allocation;
	struct mapping_slab* slab;
	uint8_t*             slots;
	size_t               count;
	if (granule <= MAPPING_SLAB_OFFSET || !pmm_alloc(&(const struct pmm_alloc_request){.size = granule}, &allocation))
		return NULL;
	slab = physical_to_virtual(allocation.address);
	memset(slab, 0, granule);
	count = (granule - MAPPING_SLAB_OFFSET) / MAPPING_SLOT_SIZE;
	if (count == 0u) {
		(void)pmm_free(allocation);
		return NULL;
	}
	slab->physical_address = allocation.address;
	slots                  = (uint8_t*)slab + MAPPING_SLAB_OFFSET;
	for (size_t i = 0u; i < count; i++) {
		struct mapping_free_slot* slot = (void*)(slots + i * MAPPING_SLOT_SIZE);
		slot->next                     = slab->free_slots;
		slab->free_slots               = slot;
	}
	return slab;
}

static struct mapping_slab* mapping_slab(const struct mapping* mapping, size_t granule) {
	uintptr_t physical = (uintptr_t)mapping - boot_info.direct_map_offset;
	physical &= ~(uintptr_t)(granule - 1u);
	return physical_to_virtual(physical);
}

static struct mapping* mapping_alloc(void) {
	struct irq_state     state = spinlock_lock_irqsave(&mapping_allocator_lock);
	struct mapping_slab* slab;
	struct mapping*      mapping;
	size_t               granule = mapping_granule();
	if (granule == 0u) {
		spinlock_unlock_irqrestore(&mapping_allocator_lock, state);
		return NULL;
	}
	for (slab = mapping_slabs; slab != NULL && slab->free_slots == NULL; slab = slab->next) {
	}
	if (slab == NULL) {
		slab = slab_create(granule);
		if (slab == NULL) {
			spinlock_unlock_irqrestore(&mapping_allocator_lock, state);
			return NULL;
		}
		slab->next    = mapping_slabs;
		mapping_slabs = slab;
	}
	mapping          = (struct mapping*)slab->free_slots;
	slab->free_slots = slab->free_slots->next;
	slab->used++;
	memset(mapping, 0, sizeof(*mapping));
	spinlock_unlock_irqrestore(&mapping_allocator_lock, state);
	return mapping;
}

static void mapping_free(struct mapping* mapping) {
	struct mapping_slab*      slab;
	struct mapping_slab**     link;
	struct mapping_free_slot* slot;
	struct irq_state          state;
	uintptr_t                 physical_address = 0u;
	size_t                    granule          = mapping_granule();
	bool                      release          = false;
	if (mapping == NULL || granule == 0u) hcf();
	slab  = mapping_slab(mapping, granule);
	state = spinlock_lock_irqsave(&mapping_allocator_lock);
	if (slab->used == 0u) hcf();
	memset(mapping, 0, sizeof(*mapping));
	slot             = (struct mapping_free_slot*)mapping;
	slot->next       = slab->free_slots;
	slab->free_slots = slot;
	slab->used--;
	if (slab->used == 0u) {
		for (link = &mapping_slabs; *link != NULL && *link != slab; link = &(*link)->next) {
		}
		if (*link != slab) hcf();
		*link            = slab->next;
		physical_address = slab->physical_address;
		release          = true;
	}
	spinlock_unlock_irqrestore(&mapping_allocator_lock, state);
	if (release && !pmm_free((struct pmm_extent){.address = physical_address, .size = granule})) hcf();
}

bool mapping_retain(struct mapping* mapping) {
	uint64_t current;
	if (mapping == NULL) return false;
	current = __atomic_load_n(&mapping->reference_count, __ATOMIC_ACQUIRE);
	for (;;) {
		if (current == 0u || current == UINT64_MAX) return false;
		if (__atomic_compare_exchange_n(
				&mapping->reference_count, &current, current + 1u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
			return true;
	}
}

void mapping_release(struct mapping* mapping) {
	uint64_t old;
	if (mapping == NULL) return;
	old = __atomic_fetch_sub(&mapping->reference_count, 1u, __ATOMIC_ACQ_REL);
	if (old == 0u) hcf();
	if (old == 1u) mapping_free(mapping);
}

uintptr_t mapping_address(const struct mapping* mapping) {
	return mapping == NULL ? 0u : mapping->address;
}

size_t mapping_size(const struct mapping* mapping) {
	return mapping == NULL ? 0u : mapping->size;
}

size_t mapping_guard_before(const struct mapping* mapping) {
	return mapping == NULL ? 0u : mapping->guard_before;
}

size_t mapping_guard_after(const struct mapping* mapping) {
	return mapping == NULL ? 0u : mapping->guard_after;
}

memory_access_t mapping_access(const struct mapping* mapping) {
	return mapping == NULL ? 0u : __atomic_load_n(&mapping->access, __ATOMIC_ACQUIRE);
}

enum memory_type mapping_memory_type(const struct mapping* mapping) {
	return mapping == NULL ? MEMORY_TYPE_NORMAL : mapping->memory_type;
}

static bool space_is_kernel(const struct address_space* space) {
	return space == &kernel_space;
}

static size_t translation_granule(void) {
	const struct hal_paging_info* info = hal_paging_info();
	return info == NULL ? 0u : info->minimum_leaf_size;
}

size_t address_space_minimum_mapping_size(void) {
	return translation_granule();
}

static uintptr_t reserved_start(const struct mapping* mapping) {
	return mapping->address - mapping->guard_before;
}

static uintptr_t reserved_end(const struct mapping* mapping) {
	return mapping->address + mapping->size + mapping->guard_after;
}

static int tree_height(const struct mapping* node) {
	return node == NULL ? 0 : node->height;
}

static void update_height(struct mapping* node) {
	int left     = tree_height(node->left);
	int right    = tree_height(node->right);
	node->height = (left > right ? left : right) + 1;
}

static void replace_child(struct address_space* space, struct mapping* old_node, struct mapping* new_node) {
	if (old_node->parent == NULL) space->mapping_root = new_node;
	else if (old_node->parent->left == old_node) old_node->parent->left = new_node;
	else old_node->parent->right = new_node;
	if (new_node != NULL) new_node->parent = old_node->parent;
}

static struct mapping* rotate_left(struct address_space* space, struct mapping* node) {
	struct mapping* right = node->right;
	replace_child(space, node, right);
	node->right = right->left;
	if (node->right != NULL) node->right->parent = node;
	right->left  = node;
	node->parent = right;
	update_height(node);
	update_height(right);
	return right;
}

static struct mapping* rotate_right(struct address_space* space, struct mapping* node) {
	struct mapping* left = node->left;
	replace_child(space, node, left);
	node->left = left->right;
	if (node->left != NULL) node->left->parent = node;
	left->right  = node;
	node->parent = left;
	update_height(node);
	update_height(left);
	return left;
}

static void rebalance_from(struct address_space* space, struct mapping* node) {
	while (node != NULL) {
		update_height(node);
		int balance = tree_height(node->left) - tree_height(node->right);
		if (balance > 1) {
			if (tree_height(node->left->left) < tree_height(node->left->right)) rotate_left(space, node->left);
			node = rotate_right(space, node);
		}
		else if (balance < -1) {
			if (tree_height(node->right->right) < tree_height(node->right->left)) rotate_right(space, node->right);
			node = rotate_left(space, node);
		}
		node = node->parent;
	}
}

static void tree_insert(struct address_space* space, struct mapping* mapping) {
	struct mapping *node = space->mapping_root, *parent = NULL;
	while (node != NULL) {
		parent = node;
		node   = reserved_start(mapping) < reserved_start(node) ? node->left : node->right;
	}
	mapping->parent = parent;
	mapping->height = 1;
	if (parent == NULL) space->mapping_root = mapping;
	else if (reserved_start(mapping) < reserved_start(parent)) parent->left = mapping;
	else parent->right = mapping;
	rebalance_from(space, parent);
}

static void tree_remove(struct address_space* space, struct mapping* node) {
	struct mapping* rebalance;
	if (node->left != NULL && node->right != NULL) {
		struct mapping* successor = node->right;
		while (successor->left != NULL) successor = successor->left;
		struct mapping* successor_parent = successor->parent;
		if (successor_parent != node) {
			replace_child(space, successor, successor->right);
			successor->right         = node->right;
			successor->right->parent = successor;
			rebalance                = successor_parent;
		}
		else rebalance = successor;
		replace_child(space, node, successor);
		successor->left         = node->left;
		successor->left->parent = successor;
		update_height(successor);
	}
	else {
		rebalance = node->parent;
		replace_child(space, node, node->left != NULL ? node->left : node->right);
	}
	node->parent = node->left = node->right = NULL;
	node->height                            = 0;
	rebalance_from(space, rebalance);
}

static struct mapping* lower_bound(struct address_space* space, uintptr_t start, struct mapping** out_previous) {
	struct mapping *node = space->mapping_root, *result = NULL, *previous = NULL;
	while (node != NULL) {
		if (reserved_start(node) >= start) {
			result = node;
			node   = node->left;
		}
		else {
			previous = node;
			node     = node->right;
		}
	}
	if (out_previous != NULL) *out_previous = previous;
	return result;
}

struct mapping* address_space_find_mapping_locked(struct address_space* space, uintptr_t address) {
	struct mapping *node = space == NULL ? NULL : space->mapping_root, *candidate = NULL;
	while (node != NULL) {
		if (node->address <= address) {
			candidate = node;
			node      = node->right;
		}
		else node = node->left;
	}
	return candidate != NULL && address - candidate->address < candidate->size ? candidate : NULL;
}

uint64_t address_space_hal_flags(const struct address_space* space, memory_access_t access) {
	uint64_t flags = space_is_kernel(space) ? HAL_PAGE_GLOBAL : HAL_PAGE_USER;
	if ((access & MEMORY_ACCESS_READ) != 0u) flags |= HAL_PAGE_READ;
	if ((access & MEMORY_ACCESS_WRITE) != 0u) flags |= HAL_PAGE_WRITE;
	if ((access & MEMORY_ACCESS_EXEC) != 0u) flags |= HAL_PAGE_EXEC;
	return flags;
}

bool address_space_is_initialized(const struct address_space* space) {
	return space != NULL && space->hal != NULL && space->end > space->base && !space->destroying;
}

struct address_space* address_space_kernel(void) {
	return &kernel_space;
}

struct hal_paging_space* address_space_hal(struct address_space* space) {
	return address_space_is_initialized(space) ? space->hal : NULL;
}

bool address_space_activate(struct address_space* space) {
	return address_space_is_initialized(space) && hal_paging_activate(space->hal);
}

static bool space_initialize(struct address_space* space, uintptr_t base, size_t size, struct hal_paging_space* hal) {
	uint64_t end;
	size_t   granule = translation_granule();
	if (space == NULL || hal == NULL || granule == 0u || size == 0u || (base & (granule - 1u)) != 0u ||
	    (size & (granule - 1u)) != 0u || add_overflow_u64(base, size, &end))
		return false;
	memset(space, 0, sizeof(*space));
	space->base = base;
	space->end  = (uintptr_t)end;
	space->hal  = hal;
	spinlock_init_class(&space->lock,
	                    space_is_kernel(space) ? "kernel_address_space" : "process_address_space",
	                    SPINLOCK_ORDER_VADDR,
	                    SPINLOCK_FLAG_IRQSAVE | SPINLOCK_FLAG_ALLOW_EXCEPTION);
	return true;
}

bool address_space_init(void) {
	struct hal_paging_space* hal;
	initialized = false;
	if (address_space_is_initialized(&kernel_space)) address_space_destroy(&kernel_space);
	if (!hal_paging_init() || (hal = hal_paging_kernel_space()) == NULL ||
	    !space_initialize(&kernel_space, MM_KERNEL_ADDRESS_SPACE_BASE, MM_KERNEL_ADDRESS_SPACE_SIZE, hal))
		return false;
	initialized = true;
	return true;
}

bool address_space_create_process(struct address_space* space) {
	struct hal_paging_space* hal;
	if (!initialized || space == NULL || space == &kernel_space || !hal_paging_space_create(&hal)) return false;
	if (!space_initialize(space, address_space_minimum_mapping_size(), MM_USER_ADDRESS_SPACE_SIZE, hal)) {
		hal_paging_space_destroy(hal);
		return false;
	}
	return true;
}

static bool access_valid(memory_access_t access) {
	return (access & ~MEMORY_ACCESS_VALID_MASK) == 0u;
}

static bool physical_layout_compatible(struct memory* memory, size_t size, size_t granule) {
	size_t    cursor            = 0u;
	size_t    boundary_unit     = SIZE_MAX;
	uintptr_t boundary_physical = 0u;
	while (cursor < size) {
		struct memory_span span;
		size_t             end;
		if (!memory_query(memory, cursor, size - cursor, &span) || span.offset != cursor || span.size == 0u ||
		    span.size > size - cursor)
			return false;
		end = cursor + span.size;
		if (span.kind == MEMORY_SPAN_PRESENT) {
			size_t    within = cursor & (granule - 1u);
			size_t    unit   = cursor - within;
			uintptr_t physical;
			if (span.physical_address < within) return false;
			physical = span.physical_address - within;
			if ((physical & (granule - 1u)) != 0u || (boundary_unit == unit && boundary_physical != physical))
				return false;
			if ((end & (granule - 1u)) != 0u) {
				size_t boundary = end & ~(granule - 1u);
				size_t advance  = boundary - unit;

				if (advance > UINTPTR_MAX - physical) return false;

				boundary_unit     = boundary;
				boundary_physical = physical + advance;
			}
			else boundary_unit = SIZE_MAX;
		}
		cursor = end;
	}
	return true;
}

static bool placement_geometry(uintptr_t address, size_t size, size_t before, size_t after, uintptr_t* out_start,
                               uintptr_t* out_end) {
	uint64_t usable_end, end;
	if (address < before || add_overflow_u64(address, size, &usable_end) || add_overflow_u64(usable_end, after, &end))
		return false;
	*out_start = address - before;
	*out_end   = (uintptr_t)end;
	return true;
}

static bool find_placement(struct address_space* space, size_t size, size_t alignment, size_t before, size_t after,
                           uintptr_t* out_address, struct mapping** out_previous, struct mapping** out_next) {
	uintptr_t gap_start = space->base;
	for (struct mapping* next = space->mapping_first;; next = next == NULL ? NULL : next->next) {
		uintptr_t gap_end = next == NULL ? space->end : reserved_start(next);
		uint64_t  before_align, address, usable_end, reservation_end;
		if (!add_overflow_u64(gap_start, before, &before_align) && align_up_u64(before_align, alignment, &address) &&
		    !add_overflow_u64(address, size, &usable_end) && !add_overflow_u64(usable_end, after, &reservation_end) &&
		    reservation_end <= gap_end) {
			*out_address  = (uintptr_t)address;
			*out_next     = next;
			*out_previous = next == NULL ? space->mapping_last : next->previous;
			return true;
		}
		if (next == NULL) return false;
		gap_start = reserved_end(next);
	}
}

bool address_space_map(struct address_space* space, const struct address_space_mapping_request* request,
                       struct mapping** out_mapping) {
	struct mapping * mapping, *previous = NULL, *next = NULL;
	struct irq_state state;
	size_t           size, granule, alignment;
	uintptr_t        address, start, end;
	if (out_mapping != NULL) *out_mapping = NULL;
	granule = translation_granule();
	if (!initialized || !address_space_is_initialized(space) || request == NULL || request->memory == NULL ||
	    out_mapping == NULL || granule == 0u || !access_valid(request->access))
		return false;
	size      = memory_size(request->memory);
	alignment = request->alignment == 0u ? granule : request->alignment;
	if (size == 0u || !memory_range_is_backing_aligned(request->memory, 0u, size, granule) || alignment < granule ||
	    (alignment & (alignment - 1u)) != 0u || (request->guard_before & (granule - 1u)) != 0u ||
	    (request->guard_after & (granule - 1u)) != 0u ||
	    (request->address != 0u && (request->address & (alignment - 1u)) != 0u) ||
	    !physical_layout_compatible(request->memory, size, granule) ||
	    ((request->access & MEMORY_ACCESS_EXEC) != 0u &&
	     (memory_type(request->memory) != MEMORY_TYPE_NORMAL || !memory_cpu_accessible(request->memory))) ||
	    (request->access != 0u &&
	     !hal_paging_mapping_supported(address_space_hal_flags(space, request->access), memory_type(request->memory))))
		return false;
	mapping = mapping_alloc();
	if (mapping == NULL || !memory_retain(request->memory)) {
		if (mapping != NULL) {
			mapping->reference_count = 1u;
			mapping_release(mapping);
		}
		return false;
	}
	mapping->memory          = request->memory;
	mapping->size            = size;
	mapping->guard_before    = request->guard_before;
	mapping->guard_after     = request->guard_after;
	mapping->memory_type     = memory_type(request->memory);
	mapping->access          = request->access;
	mapping->reference_count = 2u;
	state                    = spinlock_lock_irqsave(&space->lock);
	if (!address_space_is_initialized(space)) goto fail_locked;
	if (request->address == 0u) {
		if (!find_placement(
				space, size, alignment, request->guard_before, request->guard_after, &address, &previous, &next))
			goto fail_locked;
	}
	else {
		address = request->address;
		if (!placement_geometry(address, size, request->guard_before, request->guard_after, &start, &end) ||
		    start < space->base || end > space->end)
			goto fail_locked;
		next = lower_bound(space, start, &previous);
		if ((previous != NULL && reserved_end(previous) > start) || (next != NULL && reserved_start(next) < end))
			goto fail_locked;
	}
	mapping->address  = address;
	mapping->owner    = space;
	mapping->previous = previous;
	mapping->next     = next;
	if (previous != NULL) previous->next = mapping;
	else space->mapping_first = mapping;
	if (next != NULL) next->previous = mapping;
	else space->mapping_last = mapping;
	tree_insert(space, mapping);
	space->mapping_count++;
	spinlock_unlock_irqrestore(&space->lock, state);
	*out_mapping = mapping;
	return true;
fail_locked:
	spinlock_unlock_irqrestore(&space->lock, state);
	memory_release(request->memory);
	mapping->memory = NULL;
	mapping_release(mapping);
	mapping_release(mapping);
	return false;
}

static bool mapping_belongs(const struct address_space* space, const struct mapping* mapping) {
	return mapping != NULL && mapping->owner == space;
}

bool address_space_unmap(struct address_space* space, struct mapping* mapping) {
	struct irq_state state;
	struct memory*   memory;
	if (!initialized || !address_space_is_initialized(space) || mapping == NULL) return false;
	state = spinlock_lock_irqsave(&space->lock);
	if (!mapping_belongs(space, mapping) ||
	    (mapping->access != 0u && !hal_paging_unmap(space->hal, mapping->address, mapping->size))) {
		spinlock_unlock_irqrestore(&space->lock, state);
		return false;
	}
	tree_remove(space, mapping);
	if (mapping->previous != NULL) mapping->previous->next = mapping->next;
	else space->mapping_first = mapping->next;
	if (mapping->next != NULL) mapping->next->previous = mapping->previous;
	else space->mapping_last = mapping->previous;
	space->mapping_count--;
	memory            = mapping->memory;
	mapping->memory   = NULL;
	mapping->owner    = NULL;
	mapping->previous = mapping->next = NULL;
	spinlock_unlock_irqrestore(&space->lock, state);
	memory_release(memory);
	mapping_release(mapping);
	return true;
}

bool address_space_protect(struct address_space* space, struct mapping* mapping, memory_access_t access) {
	struct irq_state state;
	if (!initialized || !address_space_is_initialized(space) || mapping == NULL || !access_valid(access)) return false;
	state = spinlock_lock_irqsave(&space->lock);
	if (!mapping_belongs(space, mapping) ||
	    ((access & MEMORY_ACCESS_EXEC) != 0u &&
	     (mapping->memory_type != MEMORY_TYPE_NORMAL || !memory_cpu_accessible(mapping->memory))) ||
	    (access != 0u && !hal_paging_mapping_supported(address_space_hal_flags(space, access), mapping->memory_type)))
		goto fail;
	if (mapping->access == access) {
		spinlock_unlock_irqrestore(&space->lock, state);
		return true;
	}
	if ((access & MEMORY_ACCESS_EXEC) != 0u && (mapping->access & MEMORY_ACCESS_EXEC) == 0u) {
		struct memory_span span;
		for (size_t offset = 0u; offset < mapping->size; offset += span.size) {
			if (!memory_query(mapping->memory, offset, mapping->size - offset, &span)) goto fail;
			if (span.kind == MEMORY_SPAN_PRESENT)
				hal_cache_sync_executable_range_all_cpus((void*)(span.physical_address + boot_info.direct_map_offset),
				                                         span.size);
		}
	}
	if (mapping->access != 0u &&
	    !(access == 0u ? hal_paging_unmap(space->hal, mapping->address, mapping->size)
	                   : hal_paging_protect(
							 space->hal, mapping->address, mapping->size, address_space_hal_flags(space, access))))
		goto fail;
	__atomic_store_n(&mapping->access, access, __ATOMIC_RELEASE);
	spinlock_unlock_irqrestore(&space->lock, state);
	return true;
fail:
	spinlock_unlock_irqrestore(&space->lock, state);
	return false;
}

static bool access_allowed(memory_access_t granted, memory_access_t requested) {
	return requested != 0u && access_valid(requested) && (granted & requested) == requested;
}

static bool present_run(struct memory* memory, size_t offset, size_t maximum_size, size_t granule,
                        uintptr_t* out_physical, size_t* out_size) {
	uintptr_t base = 0u;
	size_t    done = 0u;
	while (done < maximum_size) {
		struct memory_span span;
		if (!memory_query(memory, offset + done, maximum_size - done, &span) || span.size == 0u ||
		    span.size > maximum_size - done || span.kind != MEMORY_SPAN_PRESENT ||
		    (done != 0u && (done > UINTPTR_MAX - base || span.physical_address != base + done)))
			break;
		if (done == 0u) base = span.physical_address;
		done += span.size;
	}
	done &= ~(granule - 1u);
	if (done == 0u || (base & (granule - 1u)) != 0u) return false;
	*out_physical = base;
	*out_size     = done;
	return true;
}

static bool map_present_run_locked(struct address_space* space, struct mapping* mapping, size_t offset,
                                   size_t maximum_size, size_t required_size) {
	size_t    granule = translation_granule();
	uintptr_t physical_address;
	size_t    run_size;
	if (granule == 0u || !present_run(mapping->memory, offset, maximum_size, granule, &physical_address, &run_size))
		return false;
	if (run_size < required_size) return false;
	const struct hal_paging_map_request request = {
		.virtual_address  = mapping->address + offset,
		.physical_address = physical_address,
		.size             = run_size,
		.flags            = address_space_hal_flags(space, mapping->access),
		.memory_type      = mapping->memory_type,
	};
	if ((mapping->access & MEMORY_ACCESS_EXEC) != 0u)
		hal_cache_sync_executable_range_all_cpus((void*)(physical_address + boot_info.direct_map_offset), run_size);
	if (hal_paging_map(space->hal, &request)) return true;
	if (run_size == granule || required_size > granule) return false;
	return hal_paging_map(space->hal,
	                      &(const struct hal_paging_map_request){
							  .virtual_address  = request.virtual_address,
							  .physical_address = request.physical_address,
							  .size             = granule,
							  .flags            = request.flags,
							  .memory_type      = request.memory_type,
						  });
}

static bool map_fault_leaf_locked(struct address_space* space, struct mapping* mapping, size_t fault_offset) {
	const struct hal_paging_info* info = hal_paging_info();
	uintptr_t                     fault_address;
	if (info == NULL) return false;
	fault_address = mapping->address + fault_offset;
	for (unsigned bit = 64u; bit-- > 0u;) {
		size_t    leaf_size;
		uintptr_t virtual_address;
		uintptr_t physical_address;
		size_t    offset, present_size;
		if ((info->leaf_size_mask & (1ull << bit)) == 0u || bit >= sizeof(size_t) * 8u) continue;
		leaf_size       = (size_t)1u << bit;
		virtual_address = fault_address & ~(uintptr_t)(leaf_size - 1u);
		if (leaf_size < info->minimum_leaf_size || virtual_address < mapping->address) continue;
		offset = (size_t)(virtual_address - mapping->address);
		if (offset > fault_offset || leaf_size > mapping->size - offset || fault_offset - offset >= leaf_size ||
		    !present_run(
				mapping->memory, offset, leaf_size, info->minimum_leaf_size, &physical_address, &present_size) ||
		    present_size != leaf_size || (physical_address & (leaf_size - 1u)) != 0u)
			continue;
		if ((mapping->access & MEMORY_ACCESS_EXEC) != 0u)
			hal_cache_sync_executable_range_all_cpus((void*)(physical_address + boot_info.direct_map_offset),
			                                         leaf_size);
		if (hal_paging_map(space->hal,
		                   &(const struct hal_paging_map_request){
							   .virtual_address  = virtual_address,
							   .physical_address = physical_address,
							   .size             = leaf_size,
							   .flags            = address_space_hal_flags(space, mapping->access),
							   .memory_type      = mapping->memory_type,
						   }))
			return true;
	}
	return false;
}

bool address_space_resolve_locked(struct address_space* space, struct mapping* mapping, size_t offset) {
	size_t    granule = translation_granule();
	uintptr_t virtual_address;
	if (granule == 0u || !mapping_belongs(space, mapping) || offset > mapping->size - granule ||
	    (offset & (granule - 1u)) != 0u || mapping->access == 0u)
		return false;
	virtual_address = mapping->address + offset;
	if (hal_paging_query(space->hal, virtual_address, NULL)) return true;
	if (!memory_materialize(mapping->memory,
	                        &(const struct memory_materialize_request){
								.offset             = offset,
								.size               = granule,
								.alignment          = granule,
								.require_contiguous = true,
							}))
		return false;
	return map_fault_leaf_locked(space, mapping, offset);
}

bool address_space_resolve_fault(struct address_space* space, uintptr_t address, memory_access_t access) {
	struct irq_state state;
	struct mapping*  mapping;
	bool             ok      = false;
	size_t           granule = translation_granule();
	if (!initialized || !address_space_is_initialized(space) || granule == 0u || !access_allowed(~0u, access))
		return false;
	state   = spinlock_lock_irqsave(&space->lock);
	mapping = address_space_find_mapping_locked(space, address);
	if (mapping != NULL && access_allowed(mapping->access, access)) {
		size_t offset = (size_t)(address - mapping->address) & ~(granule - 1u);
		ok            = address_space_resolve_locked(space, mapping, offset);
	}
	spinlock_unlock_irqrestore(&space->lock, state);
	return ok;
}

bool address_space_prefault(struct address_space* space, struct mapping* mapping, size_t offset, size_t size) {
	struct irq_state state;
	size_t           granule = translation_granule();
	if (!initialized || !address_space_is_initialized(space) || mapping == NULL || granule == 0u || size == 0u ||
	    (offset & (granule - 1u)) != 0u || (size & (granule - 1u)) != 0u)
		return false;
	state = spinlock_lock_irqsave(&space->lock);
	if (!mapping_belongs(space, mapping) || mapping->access == 0u || offset > mapping->size ||
	    size > mapping->size - offset)
		goto fail;
	if (!memory_materialize(mapping->memory,
	                        &(const struct memory_materialize_request){
								.offset             = offset,
								.size               = size,
								.alignment          = granule,
								.require_contiguous = false,
							}))
		goto fail;
	for (size_t done = 0u; done < size;) {
		struct hal_paging_translation translation;
		uintptr_t                     virtual_address = mapping->address + offset + done;
		if (hal_paging_query(space->hal, virtual_address, &translation)) {
			size_t advance = granule;
			if (translation.leaf_size >= granule && (translation.leaf_size & (translation.leaf_size - 1u)) == 0u) {
				advance = translation.leaf_size - (virtual_address & (translation.leaf_size - 1u));
				if (advance > size - done) advance = size - done;
			}
			done += advance;
			continue;
		}
		if (!map_present_run_locked(space, mapping, offset + done, size - done, granule)) goto fail;
	}
	spinlock_unlock_irqrestore(&space->lock, state);
	return true;
fail:
	spinlock_unlock_irqrestore(&space->lock, state);
	return false;
}

size_t address_space_mapping_count(struct address_space* space) {
	struct irq_state state;
	size_t           count;
	if (!address_space_is_initialized(space)) return 0u;
	state = spinlock_lock_irqsave(&space->lock);
	count = space->mapping_count;
	spinlock_unlock_irqrestore(&space->lock, state);
	return count;
}

bool address_space_contains_mapping(struct address_space* space, const struct mapping* mapping) {
	struct irq_state state;
	bool             contains;
	if (!address_space_is_initialized(space) || mapping == NULL) return false;
	state    = spinlock_lock_irqsave(&space->lock);
	contains = mapping_belongs(space, mapping);
	spinlock_unlock_irqrestore(&space->lock, state);
	return contains;
}

void address_space_destroy(struct address_space* space) {
	struct irq_state         state;
	struct mapping*          mappings;
	struct hal_paging_space* hal;
	bool                     kernel;
	if (space == NULL || space->hal == NULL || space->end <= space->base) return;
	state             = spinlock_lock_irqsave(&space->lock);
	space->destroying = true;
	hal               = space->hal;
	kernel            = space_is_kernel(space);
	mappings          = space->mapping_first;
	if (kernel) {
		for (struct mapping* mapping = mappings; mapping != NULL; mapping = mapping->next)
			if (mapping->access != 0u && !hal_paging_unmap(hal, mapping->address, mapping->size)) hcf();
	}
	for (struct mapping* mapping = mappings; mapping != NULL; mapping = mapping->next) mapping->owner = NULL;
	space->base = space->end = 0u;
	space->hal               = NULL;
	space->mapping_root      = NULL;
	space->mapping_first     = NULL;
	space->mapping_last      = NULL;
	space->mapping_count     = 0u;
	spinlock_unlock_irqrestore(&space->lock, state);
	if (!kernel) hal_paging_space_destroy(hal);
	while (mappings != NULL) {
		struct mapping* next   = mappings->next;
		struct memory*  memory = mappings->memory;
		mappings->memory       = NULL;
		mappings->previous = mappings->next = NULL;
		memory_release(memory);
		mapping_release(mappings);
		mappings = next;
	}
}

static enum address_space_fault_kind classify_fault(struct address_space* space, uintptr_t address) {
	return address_space_is_initialized(space) && hal_paging_query(space->hal, address, NULL)
	           ? ADDRESS_SPACE_FAULT_PROTECTION
	           : ADDRESS_SPACE_FAULT_NOT_PRESENT;
}

bool address_space_handle_current_fault(uintptr_t address, enum address_space_fault_kind kind, memory_access_t access,
                                        bool user_mode) {
	struct thread*        current       = sched_current_thread();
	struct address_space* current_space = current == NULL ? NULL : current->address_space;
	if (kind == ADDRESS_SPACE_FAULT_UNCLASSIFIED) {
		kind = classify_fault(current_space, address);
		if (kind == ADDRESS_SPACE_FAULT_NOT_PRESENT && !user_mode && current_space != &kernel_space)
			kind = classify_fault(&kernel_space, address);
	}
	if (kind == ADDRESS_SPACE_FAULT_NOT_PRESENT) {
		if (current_space != NULL && address_space_resolve_fault(current_space, address, access)) return true;
		if (!user_mode && current_space != &kernel_space && address_space_resolve_fault(&kernel_space, address, access))
			return true;
	}
	if (!user_mode) return false;
	struct process* process = process_current();
	if (process == NULL) return false;
	uintptr_t code = kind == ADDRESS_SPACE_FAULT_PROTECTION    ? PROCESS_EXIT_MEMORY_PROTECTION
	                 : kind == ADDRESS_SPACE_FAULT_NOT_PRESENT ? PROCESS_EXIT_MEMORY_NOT_PRESENT
	                                                           : PROCESS_EXIT_MEMORY_INVALID;
	return process_terminate(process, code);
}
