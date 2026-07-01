#include <base/heap.h>
#include <base/vmm.h>
#include <runtime/heap.h>
#include <system/process.h>

uintptr_t runtime_heap_base;
size_t    runtime_heap_page_count;
size_t    runtime_heap_used_pages;
size_t    runtime_heap_page_size;
cap_id_t  runtime_heap_address_space_cap = CAP_ID_INVALID;
bool      runtime_heap_configured;

static bool runtime_heap_configure(uintptr_t base, size_t page_count, size_t page_size, cap_id_t address_space_cap) {
	if (base == 0u || page_count == 0u || page_size != VMM_PAGE_SIZE || (base & (page_size - 1u)) != 0u ||
	    page_count > SIZE_MAX / page_size || address_space_cap == CAP_ID_INVALID) {
		return false;
	}
	if (runtime_heap_configured) return false;
	runtime_heap_base              = base;
	runtime_heap_page_count        = page_count;
	runtime_heap_used_pages        = 0u;
	runtime_heap_page_size         = page_size;
	runtime_heap_address_space_cap = address_space_cap;
	runtime_heap_configured        = true;
	return true;
}

bool runtime_heap_init(const struct process_startup_info* startup) {
	struct self_info self;

	if (startup == NULL || !syscall_status_is_success(process_self_info(&self))) return false;
	if (!runtime_heap_configure(
			startup->heap_base, startup->heap_page_count, startup->page_size, self.address_space_cap)) {
		return false;
	}
	return heap_init();
}

bool runtime_heap_is_configured(void) {
	return runtime_heap_configured;
}
