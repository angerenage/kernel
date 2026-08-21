#include <base/heap.h>
#include <core/lock.h>
#include <core/memory_object.h>
#include <core/pmm.h>
#include <core/spinlock.h>
#include <core/vm_space.h>
#include <stdbool.h>
#include <stddef.h>

static struct spinlock kernel_heap_lock = SPINLOCK_INIT_CLASS("heap_lock", SPINLOCK_ORDER_HEAP, SPINLOCK_FLAG_NONE);

bool heap_grow_pages(size_t page_count, void** out_base) {
	struct memory_object* memory;
	vmm_id_t              id;
	if (out_base != NULL) *out_base = NULL;
	if (out_base == NULL || !memory_object_create_owned(page_count, &memory)) return false;
	bool mapped = vm_space_map(vm_space_kernel(),
	                           &(const struct vm_map_request){
								   .memory      = memory,
								   .page_count  = page_count,
								   .align_pages = 1u,
								   .prot        = VMM_PROT_READ | VMM_PROT_WRITE | VMM_PROT_GLOBAL,
							   },
	                           &id,
	                           out_base);
	memory_object_release(memory);
	if (!mapped) return false;
	if (vm_space_prefault(vm_space_kernel(), id, 0u, page_count)) return true;
	(void)vm_space_unmap(vm_space_kernel(), id);
	*out_base = NULL;
	return false;
}

size_t heap_page_size(void) {
	return PMM_PAGE_SIZE;
}

void heap_lock(void) {
	spinlock_lock(&kernel_heap_lock);
}

void heap_unlock(void) {
	spinlock_unlock(&kernel_heap_lock);
}
