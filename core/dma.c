#include <base/dma.h>
#include <core/address_space.h>
#include <core/dma.h>
#include <core/memory.h>
#include <core/mm.h>
#include <core/pmm.h>
#include <hal/cache.h>
#include <hal/hcf.h>
#include <libc/stdlib.h>
#include <libc/string.h>

#include "dma_internal.h"
#include "memory/address_space_internal.h"

struct dma_binding {
	uint64_t                          reference_count;
	dma_source_t                      source;
	struct address_space*             address_space;
	struct hal_iommu_attachment_state attachment;
	bool                              active;
	struct dma_binding*               next;
	uint32_t                          controller_index;
};

enum dma_controller_init_state {
	DMA_CONTROLLER_UNINITIALIZED = 0u,
	DMA_CONTROLLER_INITIALIZING,
	DMA_CONTROLLER_INITIALIZED,
};

struct dma_controller {
	struct hal_iommu_controller_descriptor descriptor;
	uint32_t                               init_state;
	struct hal_iommu_controller_state      hal;
	struct hal_iommu_info                  info;
	uint64_t                               next_context_id;
	uint32_t                               maximum_context_id;
	uint32_t*                              free_context_ids;
	size_t                                 free_context_count;
	size_t                                 free_context_capacity;
	struct dma_binding*                    bindings;
	struct spinlock                        lock;
};

static struct dma_controller* dma_controllers;
static size_t                 dma_controller_count;
static bool                   dma_initialized;

static void* physical_to_virtual(uintptr_t address) {
	return (void*)(address + boot_info.direct_map_offset);
}

static struct dma_controller* dma_controller_at_index(uint32_t index) {
	return dma_initialized && index < dma_controller_count ? &dma_controllers[index] : NULL;
}
static bool dma_context_growth_capacity(size_t current_capacity, size_t required_capacity, size_t* out_capacity) {
	size_t capacity;
	if (out_capacity == NULL || required_capacity == 0u) return false;
	capacity = current_capacity == 0u ? 16u : current_capacity;
	while (capacity < required_capacity) {
		if (capacity > SIZE_MAX / 2u) {
			capacity = required_capacity;
			break;
		}
		capacity *= 2u;
	}
	if (capacity > SIZE_MAX / sizeof(uint32_t)) return false;
	*out_capacity = capacity;
	return true;
}

static bool dma_context_push(struct dma_controller* controller, uint32_t context_id) {
	if (controller == NULL || context_id == 0u || (uint64_t)context_id >= controller->next_context_id ||
	    context_id > controller->maximum_context_id)
		return false;
	for (size_t index = 0u; index < controller->free_context_count; index++)
		if (controller->free_context_ids[index] == context_id) return false;
	/* Capacity is reserved before an ID is first handed out, so release cannot allocate. */
	if (controller->free_context_count >= controller->free_context_capacity) return false;
	controller->free_context_ids[controller->free_context_count++] = context_id;
	return true;
}

static size_t context_limit(uint8_t bits) {
	return bits >= 32u ? UINT32_MAX : ((size_t)1u << bits) - 1u;
}

static uintptr_t device_end(const struct hal_iommu_info* info) {
	if (info == NULL || info->minimum_leaf_size == 0u) return 0u;
	if (info->io_address_bits < sizeof(uintptr_t) * 8u) return (uintptr_t)1u << info->io_address_bits;
	return UINTPTR_MAX & ~((uintptr_t)info->minimum_leaf_size - 1u);
}

static unsigned size_shift(size_t size) {
	unsigned shift = 0u;
	while (((size_t)1u << shift) != size) shift++;
	return shift;
}

static bool dma_controller_is_initialized(const struct dma_controller* controller) {
	return controller != NULL &&
	       __atomic_load_n(&controller->init_state, __ATOMIC_ACQUIRE) == DMA_CONTROLLER_INITIALIZED;
}

static bool dma_controller_initialize(struct dma_controller* controller) {
	uint32_t state;
	if (controller == NULL) return false;
	for (;;) {
		state = __atomic_load_n(&controller->init_state, __ATOMIC_ACQUIRE);
		if (state == DMA_CONTROLLER_INITIALIZED) return true;
		if (state == DMA_CONTROLLER_INITIALIZING) {
			spinlock_relax();
			continue;
		}
		uint32_t expected = DMA_CONTROLLER_UNINITIALIZED;
		if (__atomic_compare_exchange_n(&controller->init_state,
		                                &expected,
		                                DMA_CONTROLLER_INITIALIZING,
		                                false,
		                                __ATOMIC_ACQ_REL,
		                                __ATOMIC_ACQUIRE))
			break;
	}
	if (!hal_iommu_controller_init(&controller->hal, &controller->descriptor, &controller->info)) {
		__atomic_store_n(&controller->init_state, DMA_CONTROLLER_UNINITIALIZED, __ATOMIC_RELEASE);
		return false;
	}
	if (controller->info.minimum_leaf_size == 0u ||
	    (controller->info.minimum_leaf_size & (controller->info.minimum_leaf_size - 1u)) != 0u ||
	    controller->info.io_address_bits == 0u || controller->info.io_address_bits > 64u ||
	    controller->info.physical_address_bits == 0u ||
	    controller->info.physical_address_bits > sizeof(uintptr_t) * 8u || controller->info.context_id_bits == 0u ||
	    controller->info.context_id_bits > 32u || controller->info.source_id_bits == 0u ||
	    controller->info.source_id_bits > 32u || controller->info.kind != controller->descriptor.kind ||
	    (controller->info.leaf_size_mask & (1ull << size_shift(controller->info.minimum_leaf_size))) == 0u) {
		hal_iommu_controller_deinit(&controller->hal);
		controller->info = (struct hal_iommu_info){0};
		__atomic_store_n(&controller->init_state, DMA_CONTROLLER_UNINITIALIZED, __ATOMIC_RELEASE);
		return false;
	}
	controller->maximum_context_id = (uint32_t)context_limit(controller->info.context_id_bits);
	controller->next_context_id    = 1u;
	__atomic_store_n(&controller->init_state, DMA_CONTROLLER_INITIALIZED, __ATOMIC_RELEASE);
	return true;
}
static struct dma_binding* dma_binding_find(struct dma_controller* controller, dma_source_t source) {
	for (struct dma_binding* binding = controller == NULL ? NULL : controller->bindings; binding != NULL;
	     binding                     = binding->next)
        if (binding->source == source && binding->active) return binding;
	return NULL;
}

bool dma_init(void) {
	size_t count = hal_iommu_controller_count();
	if (dma_initialized) return true;
	if (count > UINT32_MAX) return false;
	dma_controllers = count == 0u ? NULL : calloc(count, sizeof(*dma_controllers));
	if (count != 0u && dma_controllers == NULL) return false;
	dma_controller_count = count;
	for (size_t index = 0u; index < count; index++) {
		if (!hal_iommu_controller_at(index, &dma_controllers[index].descriptor) ||
		    dma_controllers[index].descriptor.register_address == 0u) {
			free(dma_controllers);
			dma_controllers      = NULL;
			dma_controller_count = 0u;
			return false;
		}
		for (size_t previous = 0u; previous < index; previous++)
			if (dma_controllers[previous].descriptor.register_address ==
			    dma_controllers[index].descriptor.register_address) {
				free(dma_controllers);
				dma_controllers      = NULL;
				dma_controller_count = 0u;
				return false;
			}
		spinlock_init_class(
			&dma_controllers[index].lock, "dma_controller", SPINLOCK_ORDER_DMA_CONTROLLER, SPINLOCK_FLAG_IRQSAVE);
	}
	dma_initialized = true;
	return true;
}

bool dma_available(void) {
	return dma_initialized && dma_controller_count != 0u;
}

static bool dma_controller_from_register_address(uint64_t controller_register_address, uint32_t* out_controller_index) {
	if (!dma_initialized || out_controller_index == NULL || controller_register_address == 0u) return false;
	for (uint32_t index = 0u; index < dma_controller_count; index++) {
		if (dma_controllers[index].descriptor.register_address == (uintptr_t)controller_register_address) {
			*out_controller_index = index;
			return true;
		}
	}
	return false;
}

static bool dma_controller_decode_source(dma_source_t source, uint32_t* out_controller_index,
                                         uint32_t* out_local_source_id) {
	if (out_controller_index == NULL || out_local_source_id == NULL || source == DMA_SOURCE_INVALID) return false;
	*out_controller_index = (uint32_t)(source >> 32u);
	*out_local_source_id  = (uint32_t)source;
	return *out_controller_index < dma_controller_count;
}

const struct hal_iommu_info* dma_controller_info(uint32_t controller_index) {
	struct dma_controller* controller = dma_controller_at_index(controller_index);
	return dma_controller_is_initialized(controller) ? &controller->info : NULL;
}

struct hal_iommu_controller_state* dma_controller_state(uint32_t controller_index) {
	struct dma_controller* controller = dma_controller_at_index(controller_index);
	return dma_controller_is_initialized(controller) ? &controller->hal : NULL;
}

bool dma_controller_release_context(uint32_t controller_index, uint32_t context_id) {
	struct dma_controller* controller = dma_controller_at_index(controller_index);
	if (!dma_controller_is_initialized(controller) || context_id == 0u || context_id > controller->maximum_context_id)
		return false;
	struct irq_state state = spinlock_lock_irqsave(&controller->lock);
	bool             ret   = dma_context_push(controller, context_id);
	spinlock_unlock_irqrestore(&controller->lock, state);
	return ret;
}

static bool dma_controller_acquire_context(uint32_t controller_index, uint32_t* out_context_id) {
	struct dma_controller* controller = dma_controller_at_index(controller_index);
	if (!dma_controller_is_initialized(controller) || out_context_id == NULL) return false;

	for (;;) {
		size_t           required_capacity;
		size_t           new_capacity;
		struct irq_state state = spinlock_lock_irqsave(&controller->lock);
		if (controller->free_context_count != 0u) {
			*out_context_id = controller->free_context_ids[--controller->free_context_count];
			spinlock_unlock_irqrestore(&controller->lock, state);
			return true;
		}
		if (controller->next_context_id == 0u || controller->next_context_id > controller->maximum_context_id ||
		    controller->next_context_id > SIZE_MAX) {
			spinlock_unlock_irqrestore(&controller->lock, state);
			return false;
		}
		required_capacity = (size_t)controller->next_context_id;
		if (required_capacity <= controller->free_context_capacity) {
			*out_context_id = (uint32_t)controller->next_context_id++;
			spinlock_unlock_irqrestore(&controller->lock, state);
			return true;
		}
		if (!dma_context_growth_capacity(controller->free_context_capacity, required_capacity, &new_capacity)) {
			spinlock_unlock_irqrestore(&controller->lock, state);
			return false;
		}
		spinlock_unlock_irqrestore(&controller->lock, state);

		/* Heap allocation must never happen while the DMA controller lock is held. */
		uint32_t* replacement = malloc(new_capacity * sizeof(*replacement));
		if (replacement == NULL) return false;

		uint32_t* retired = NULL;
		state             = spinlock_lock_irqsave(&controller->lock);
		if (controller->free_context_capacity < required_capacity) {
			if (controller->free_context_count != 0u)
				memcpy(
					replacement, controller->free_context_ids, controller->free_context_count * sizeof(*replacement));
			retired                           = controller->free_context_ids;
			controller->free_context_ids      = replacement;
			controller->free_context_capacity = new_capacity;
			replacement                       = NULL;
		}
		spinlock_unlock_irqrestore(&controller->lock, state);
		free(retired);
		free(replacement);
	}
}

static bool dma_cache_sync_range(enum dma_sync_target target, void* address, size_t size) {
	switch (target) {
	case DMA_SYNC_FOR_DEVICE:
		return hal_cache_sync_for_device(address, size);
	case DMA_SYNC_FOR_CPU:
		return hal_cache_sync_for_cpu(address, size);
	}
	return false;
}

bool dma_mapping_sync(struct address_space* device_space, struct mapping* mapping, size_t offset, size_t size,
                      enum dma_sync_target target) {
	struct memory* memory;
	size_t         mapping_size;
	size_t         granule;
	size_t         end;
	size_t         cursor;
	bool           result = false;

	if (!dma_initialized || device_space == NULL || mapping == NULL || size == 0u ||
	    (target != DMA_SYNC_FOR_DEVICE && target != DMA_SYNC_FOR_CPU) || !address_space_device_retain(device_space))
		return false;

	struct irq_state state = spinlock_lock_irqsave(&device_space->lock);
	granule                = address_space_minimum_mapping_size(device_space);
	mapping_size           = mapping->size;
	if (!address_space_is_initialized(device_space) || device_space->kind != ADDRESS_SPACE_KIND_DEVICE ||
	    mapping->owner != device_space || mapping->memory == NULL || mapping->access == 0u || granule == 0u ||
	    (offset & (granule - 1u)) != 0u || (size & (granule - 1u)) != 0u || offset > mapping_size ||
	    size > mapping_size - offset || !memory_retain(mapping->memory)) {
		spinlock_unlock_irqrestore(&device_space->lock, state);
		address_space_device_release(device_space);
		return false;
	}
	memory = mapping->memory;
	end    = offset + size;
	spinlock_unlock_irqrestore(&device_space->lock, state);
	address_space_device_release(device_space);

	if (!memory_can_transfer(memory)) goto done;

	cursor = offset;
	while (cursor < end) {
		struct memory_span span;
		if (!memory_query(memory, cursor, end - cursor, &span) || span.kind != MEMORY_SPAN_PRESENT || span.size == 0u ||
		    span.size > end - cursor || span.physical_address > UINTPTR_MAX - boot_info.direct_map_offset)
			goto done;
		cursor += span.size;
	}

	cursor = offset;
	while (cursor < end) {
		struct memory_span span;
		if (!memory_query(memory, cursor, end - cursor, &span) || span.kind != MEMORY_SPAN_PRESENT || span.size == 0u ||
		    span.size > end - cursor)
			goto done;
		if (!dma_cache_sync_range(target, physical_to_virtual(span.physical_address), span.size)) goto done;
		cursor += span.size;
	}
	result = true;

done:
	memory_release(memory);
	return result;
}

bool dma_source_resolve(uint64_t controller_register_address, uint32_t local_source_id, dma_source_t* out_source) {
	uint32_t controller_index;
	if (!dma_initialized || out_source == NULL || controller_register_address == 0u ||
	    !dma_controller_from_register_address(controller_register_address, &controller_index))
		return false;
	*out_source = ((dma_source_t)controller_index << 32u) | local_source_id;
	return true;
}

bool dma_address_space_create(dma_source_t compatibility_source, struct address_space** out_space) {
	uint32_t               controller_index;
	uint32_t               local_source_id;
	uint32_t               context_id;
	struct dma_controller* controller;
	struct address_space*  space;
	struct pmm_extent      allocation;
	size_t                 granule;
	if (out_space != NULL) *out_space = NULL;
	if (out_space == NULL || !dma_controller_decode_source(compatibility_source, &controller_index, &local_source_id))
		return false;
	controller = dma_controller_at_index(controller_index);
	if (controller == NULL || !dma_controller_initialize(controller)) return false;
	if (controller->info.source_id_bits < 32u && local_source_id >= (1u << controller->info.source_id_bits))
		return false;
	if (!dma_controller_acquire_context(controller_index, &context_id)) return false;
	granule = pmm_info() == NULL ? 0u : pmm_info()->allocation_granule;
	if (granule == 0u ||
	    !pmm_alloc(&(const struct pmm_alloc_request){.size = granule, .alignment = granule}, &allocation)) {
		if (!dma_controller_release_context(controller_index, context_id)) hcf();
		return false;
	}
	space = physical_to_virtual(allocation.address);
	memset(space, 0, sizeof(*space));
	space->kind                            = ADDRESS_SPACE_KIND_DEVICE;
	space->base                            = controller->info.minimum_leaf_size;
	space->end                             = device_end(&controller->info);
	space->backend.device.reference_count  = 1u;
	space->backend.device.controller_index = controller_index;
	space->backend.device.context_id       = context_id;
	spinlock_init_class(&space->lock,
	                    "device_address_space",
	                    SPINLOCK_ORDER_VADDR,
	                    SPINLOCK_FLAG_IRQSAVE | SPINLOCK_FLAG_ALLOW_EXCEPTION);
	if (space->end <= space->base ||
	    !hal_iommu_space_init(&controller->hal, space->backend.device.context_id, &space->backend.device.hal)) {
		if (!dma_controller_release_context(controller_index, space->backend.device.context_id)) hcf();
		(void)pmm_free(allocation);
		return false;
	}
	*out_space = space;
	return true;
}

static void dma_binding_free(struct dma_binding* binding) {
	free(binding);
}

bool dma_binding_retain(struct dma_binding* binding) {
	uint64_t current;
	if (binding == NULL) return false;
	current = __atomic_load_n(&binding->reference_count, __ATOMIC_ACQUIRE);
	for (;;) {
		if (current == 0u || current == UINT64_MAX) return false;
		if (__atomic_compare_exchange_n(
				&binding->reference_count, &current, current + 1u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
			return true;
	}
}

void dma_binding_release(struct dma_binding* binding) {
	uint64_t old;
	if (binding == NULL) return;
	old = __atomic_fetch_sub(&binding->reference_count, 1u, __ATOMIC_ACQ_REL);
	if (old == 0u) hcf();
	if (old == 1u) {
		if (binding->active) hcf();
		dma_binding_free(binding);
	}
}

dma_source_t dma_binding_source(const struct dma_binding* binding) {
	return binding == NULL ? DMA_SOURCE_INVALID : binding->source;
}

struct address_space* dma_binding_address_space(struct dma_binding* binding) {
	struct dma_controller* controller;
	struct address_space*  space = NULL;
	if (binding == NULL) return NULL;
	controller = dma_controller_at_index(binding->controller_index);
	if (controller == NULL) return NULL;
	struct irq_state state = spinlock_lock_irqsave(&controller->lock);
	if (binding->active && binding->address_space != NULL && address_space_device_retain(binding->address_space))
		space = binding->address_space;
	spinlock_unlock_irqrestore(&controller->lock, state);
	return space;
}

bool dma_binding_is_active(const struct dma_binding* binding) {
	struct dma_controller* controller;
	bool                   active;
	if (binding == NULL) return false;
	controller = dma_controller_at_index(binding->controller_index);
	if (controller == NULL) return false;
	struct irq_state state = spinlock_lock_irqsave(&controller->lock);
	active                 = binding->active;
	spinlock_unlock_irqrestore(&controller->lock, state);
	return active;
}

bool dma_bind(dma_source_t source, struct address_space* device_space, struct dma_binding** out_binding) {
	uint32_t               controller_index;
	uint32_t               local_source_id;
	struct dma_controller* controller;
	struct dma_binding*    binding;
	if (out_binding != NULL) *out_binding = NULL;
	if (out_binding == NULL || device_space == NULL || device_space->kind != ADDRESS_SPACE_KIND_DEVICE ||
	    !dma_controller_decode_source(source, &controller_index, &local_source_id))
		return false;
	controller = dma_controller_at_index(controller_index);
	if (!dma_controller_is_initialized(controller) ||
	    device_space->backend.device.controller_index != controller_index ||
	    (controller->info.source_id_bits < 32u && local_source_id >= (1u << controller->info.source_id_bits)))
		return false;

	/* Binding metadata allocation may grow the kernel heap and must happen outside the controller lock. */
	binding = calloc(1u, sizeof(*binding));
	if (binding == NULL) return false;
	if (!address_space_device_retain(device_space)) {
		free(binding);
		return false;
	}
	binding->reference_count  = 2u;
	binding->source           = source;
	binding->address_space    = device_space;
	binding->controller_index = controller_index;

	struct irq_state state = spinlock_lock_irqsave(&controller->lock);
	if (dma_binding_find(controller, source) != NULL ||
	    !hal_iommu_attach(&controller->hal, &device_space->backend.device.hal, local_source_id, &binding->attachment)) {
		spinlock_unlock_irqrestore(&controller->lock, state);
		address_space_device_release(device_space);
		free(binding);
		return false;
	}
	binding->active      = true;
	binding->next        = controller->bindings;
	controller->bindings = binding;
	spinlock_unlock_irqrestore(&controller->lock, state);
	*out_binding = binding;
	return true;
}

bool dma_unbind(struct dma_binding* binding) {
	struct dma_controller* controller;
	struct dma_binding**   link;
	uint32_t               controller_index;
	uint32_t               local_source_id;
	struct address_space*  space = NULL;

	if (binding == NULL) return false;
	controller_index = binding->controller_index;
	local_source_id  = (uint32_t)binding->source;
	controller       = dma_controller_at_index(controller_index);
	if (!dma_controller_is_initialized(controller)) return false;

	struct irq_state state = spinlock_lock_irqsave(&controller->lock);
	if (!binding->active) {
		spinlock_unlock_irqrestore(&controller->lock, state);
		return false;
	}
	for (link = &controller->bindings; *link != NULL && *link != binding; link = &(*link)->next) {
	}
	if (*link != binding || !hal_iommu_detach(&controller->hal, local_source_id, &binding->attachment)) {
		spinlock_unlock_irqrestore(&controller->lock, state);
		return false;
	}
	*link                  = binding->next;
	binding->next          = NULL;
	binding->active        = false;
	space                  = binding->address_space;
	binding->address_space = NULL;
	spinlock_unlock_irqrestore(&controller->lock, state);

	if (space != NULL) address_space_device_release(space);
	/* Drop the core registry's structural Binding reference. */
	dma_binding_release(binding);
	return true;
}

bool dma_binding_recover(dma_source_t source, struct dma_binding** out_binding) {
	uint32_t               controller_index;
	uint32_t               local_source_id;
	struct dma_controller* controller;
	struct dma_binding*    binding;
	if (out_binding != NULL) *out_binding = NULL;
	if (out_binding == NULL || !dma_controller_decode_source(source, &controller_index, &local_source_id)) return false;
	controller = dma_controller_at_index(controller_index);
	if (controller == NULL) return false;
	struct irq_state state = spinlock_lock_irqsave(&controller->lock);
	binding                = dma_binding_find(controller, source);
	if (binding == NULL) {
		spinlock_unlock_irqrestore(&controller->lock, state);
		return false;
	}
	if (!dma_binding_retain(binding)) {
		spinlock_unlock_irqrestore(&controller->lock, state);
		return false;
	}
	*out_binding = binding;
	spinlock_unlock_irqrestore(&controller->lock, state);
	return true;
}
