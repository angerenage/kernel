#include <stdbool.h>
#include <stddef.h>

#include <base/heap.h>
#include <base/math.h>
#include <core/address_space.h>
#include <core/memory.h>
#include <core/spinlock.h>

static struct spinlock kernel_heap_lock =
		(struct spinlock)SPINLOCK_INIT_CLASS("heap_lock", SPINLOCK_ORDER_HEAP, SPINLOCK_FLAG_IRQSAVE);

bool heap_grow_region(size_t minimum_size, void** out_base, size_t* out_size) {
	struct memory* memory;
	struct mapping* mapping;
	if (out_base != NULL) *out_base = NULL;
	if (out_size != NULL) *out_size = 0u;
	size_t granule = address_space_minimum_mapping_size(address_space_kernel());
	size_t size;
	if (out_base == NULL || out_size == NULL || granule == 0u || !align_up_size(minimum_size, granule, &size) ||
			!memory_create_anonymous(size, &memory))
		return false;
	bool mapped = address_space_map(address_space_kernel(),
			&(const struct address_space_mapping_request){
				.memory = memory,
				.access = MEMORY_ACCESS_READ | MEMORY_ACCESS_WRITE,
			},
			&mapping);
	memory_release(memory);
	if (!mapped) return false;
	*out_base = (void*)mapping_address(mapping);
	*out_size = size;
	if (address_space_prefault(address_space_kernel(), mapping, 0u, mapping_size(mapping))) {
		mapping_release(mapping);
		return true;
	}
	(void)address_space_unmap(address_space_kernel(), mapping);
	mapping_release(mapping);
	*out_base = NULL;
	*out_size = 0u;
	return false;
}

size_t heap_growth_granule(void) {
	return address_space_minimum_mapping_size(address_space_kernel());
}

struct heap_lock_state heap_lock(void) {
	struct irq_state irq_state = spinlock_lock_irqsave(&kernel_heap_lock);
	return (struct heap_lock_state){.interrupts_enabled = irq_state.enabled};
}

void heap_unlock(struct heap_lock_state state) {
	spinlock_unlock_irqrestore(&kernel_heap_lock, (struct irq_state){.enabled = state.interrupts_enabled});
}
