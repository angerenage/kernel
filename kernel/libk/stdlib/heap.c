#include <base/heap.h>
#include <base/vmm.h>
#include <core/address_space.h>
#include <core/lock.h>
#include <core/memory.h>
#include <core/pmm.h>
#include <core/spinlock.h>
#include <stdbool.h>
#include <stddef.h>

static struct spinlock kernel_heap_lock = SPINLOCK_INIT_CLASS("heap_lock", SPINLOCK_ORDER_HEAP, SPINLOCK_FLAG_NONE);

bool heap_grow_pages(size_t page_count, void** out_base) {
	struct memory*  memory;
	struct mapping* mapping;
	if (out_base != NULL) *out_base = NULL;
	if (out_base == NULL || page_count > SIZE_MAX / VMM_PAGE_SIZE ||
	    !memory_create_anonymous(page_count * VMM_PAGE_SIZE, &memory))
		return false;
	bool mapped = address_space_map(address_space_kernel(),
	                                &(const struct address_space_mapping_request){
										.memory = memory,
										.access = MAPPING_ACCESS_READ | MAPPING_ACCESS_WRITE,
									},
	                                &mapping);
	memory_release(memory);
	if (!mapped) return false;
	*out_base = (void*)mapping_address(mapping);
	if (address_space_prefault(address_space_kernel(), mapping, 0u, mapping_size(mapping))) {
		mapping_release(mapping);
		return true;
	}
	(void)address_space_unmap(address_space_kernel(), mapping);
	mapping_release(mapping);
	*out_base = NULL;
	return false;
}

size_t heap_page_size(void) {
	return VMM_PAGE_SIZE;
}

void heap_lock(void) {
	spinlock_lock(&kernel_heap_lock);
}

void heap_unlock(void) {
	spinlock_unlock(&kernel_heap_lock);
}
