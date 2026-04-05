#include <core/kheap.h>
#include <core/vmm.h>
#include <stdbool.h>
#include <stddef.h>

static bool kheap_vmm_grow(size_t page_count, void** out_base) {
	struct vmm_alloc_params params = {
		.page_count  = page_count,
		.align_pages = 1,
		.prot        = VMM_PROT_READ | VMM_PROT_WRITE | VMM_PROT_GLOBAL,
		.kind        = VMM_KIND_HEAP,
		.map_flags   = 0,
	};

	return vmm_alloc(&params, NULL, out_base);
}

bool kheap_init(void) {
	return kheap_init_with_grower(kheap_vmm_grow);
}
