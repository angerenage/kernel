#include <base/heap.h>
#include <base/vmm.h>
#include <runtime/heap.h>
#include <system/capability.h>
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
	cap_id_t                        memory_cap = CAP_ID_INVALID;
	struct address_space_map_result mapped     = {.mapping_cap = CAP_ID_INVALID};

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

	const struct memory_create_params create_params = {.page_count = page_count};
	if (!syscall_status_is_success(memory_create(&create_params, &memory_cap))) return false;
	const struct memory_map_params params = {
		.page_count = page_count, .align_pages = 1u, .prot = VMM_PROT_READ | VMM_PROT_WRITE};
	if (!syscall_status_is_success(address_space_map(runtime_heap_address_space_cap, memory_cap, &params, &mapped)) ||
	    mapped.mapping.base == NULL || mapped.mapping.page_count != page_count) {
		goto cleanup;
	}
	if (!syscall_status_is_success(cap_drop(memory_cap))) goto cleanup;
	memory_cap = CAP_ID_INVALID;
	if (!syscall_status_is_success(cap_drop(mapped.mapping_cap))) goto cleanup;
	mapped.mapping_cap = CAP_ID_INVALID;
	*out_base          = mapped.mapping.base;
	return true;

cleanup:
	if (mapped.mapping_cap != CAP_ID_INVALID) (void)mapping_unmap(mapped.mapping_cap);
	if (memory_cap != CAP_ID_INVALID) (void)cap_drop(memory_cap);
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
