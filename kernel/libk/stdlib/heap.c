#include <base/heap.h>
#include <core/lock.h>
#include <core/pmm.h>
#include <core/spinlock.h>
#include <core/vaddr_alloc.h>
#include <core/vmm.h>
#include <stdbool.h>
#include <stddef.h>

static struct spinlock kernel_kheap_lock = SPINLOCK_INIT_CLASS("kheap_lock", SPINLOCK_ORDER_KHEAP, SPINLOCK_FLAG_NONE);

bool heap_grow_pages(size_t page_count, void** out_base) {
	struct vmm_alloc_params params = {
		.page_count  = page_count,
		.align_pages = 1u,
		.prot        = VMM_PROT_READ | VMM_PROT_WRITE | VMM_PROT_GLOBAL,
		.kind        = VMM_KIND_HEAP,
		.guard_pages = 0u,
		.map_flags   = 0u,
	};

	return vmm_alloc(address_space_kernel(), &params, NULL, out_base);
}

size_t heap_page_size(void) {
	return PMM_PAGE_SIZE;
}

void heap_lock(void) {
	spinlock_lock(&kernel_kheap_lock);
}

void heap_unlock(void) {
	spinlock_unlock(&kernel_kheap_lock);
}
