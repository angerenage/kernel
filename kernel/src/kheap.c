#include <core/kheap.h>
#include <core/vmm.h>
#include <stdbool.h>
#include <stddef.h>

bool kheap_grow_pages(size_t page_count, void** out_base) {
	struct vmm_alloc_params params = {
		.page_count  = page_count,
		.align_pages = 1u,
		.prot        = VMM_PROT_READ | VMM_PROT_WRITE | VMM_PROT_GLOBAL,
		.kind        = VMM_KIND_HEAP,
		.guard_pages = 0u,
		.map_flags   = 0u,
	};

	return vmm_alloc(&params, NULL, out_base);
}
