#include <base/heap.h>
#include <base/vmm.h>
#include <runtime/heap.h>
#include <system/memory.h>

static bool userspace_heap_state_locked;
static bool userspace_heap_locked;

static void userspace_heap_state_lock(void) {
	while (__atomic_test_and_set(&userspace_heap_state_locked, __ATOMIC_ACQUIRE)) {
		__asm__ volatile("" ::: "memory");
	}
}

static void userspace_heap_state_unlock(void) {
	__atomic_clear(&userspace_heap_state_locked, __ATOMIC_RELEASE);
}

bool heap_grow_pages(size_t page_count, void** out_base) {
	cap_id_t        allocation_cap = CAP_ID_INVALID;
	cap_id_t        mapping_cap    = CAP_ID_INVALID;
	struct vmm_info mapping;

	if (out_base == NULL || page_count == 0u) return false;
	*out_base = NULL;

	userspace_heap_state_lock();
	if (!runtime_heap_is_configured() || page_count > SIZE_MAX - runtime_heap_used_pages) {
		userspace_heap_state_unlock();
		return false;
	}
	if (page_count <= runtime_heap_page_count - runtime_heap_used_pages) {
		*out_base = (void*)(runtime_heap_base + runtime_heap_used_pages * runtime_heap_page_size);
		runtime_heap_used_pages += page_count;
		userspace_heap_state_unlock();
		return true;
	}
	userspace_heap_state_unlock();

	if (!memory_allocate(page_count, VMM_PROT_READ | VMM_PROT_WRITE | VMM_PROT_USER, VMM_KIND_HEAP, &allocation_cap)) {
		return false;
	}
	if (!address_space_map(runtime_heap_address_space_cap, allocation_cap, &mapping_cap)) goto cleanup;
	if (!mapping_get_info(mapping_cap, &mapping) || mapping.base == NULL || mapping.page_count != page_count)
		goto cleanup;

	*out_base = mapping.base;
	return true;

cleanup:
	if (mapping_cap != CAP_ID_INVALID) (void)mapping_unmap(mapping_cap);
	if (allocation_cap != CAP_ID_INVALID) (void)allocation_free(allocation_cap);
	return false;
}

size_t heap_page_size(void) {
	return runtime_heap_page_size;
}

void heap_lock(void) {
	while (__atomic_test_and_set(&userspace_heap_locked, __ATOMIC_ACQUIRE)) {
		__asm__ volatile("" ::: "memory");
	}
}

void heap_unlock(void) {
	__atomic_clear(&userspace_heap_locked, __ATOMIC_RELEASE);
}
