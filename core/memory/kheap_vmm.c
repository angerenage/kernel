#include <core/kheap.h>
#include <core/vmm.h>
#include <stdbool.h>
#include <stddef.h>

static bool kheap_vmm_grow(size_t page_count, void** out_base) {
	return vmm_alloc_pages(page_count, 1, VMM_PAGE_WRITE | VMM_PAGE_GLOBAL, out_base);
}

bool kheap_init(void) {
	return kheap_init_with_grower(kheap_vmm_grow);
}
