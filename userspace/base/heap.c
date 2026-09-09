#include <base/heap.h>
#include <base/math.h>
#include <runtime/heap.h>
#include <system/capability.h>
#include <system/memory.h>

static bool userspace_heap_state_locked;
static bool userspace_heap_locked;

static void userspace_heap_state_lock(void) {
	while (__atomic_test_and_set(&userspace_heap_state_locked, __ATOMIC_ACQUIRE)) __asm__ volatile("" ::: "memory");
}

static void userspace_heap_state_unlock(void) {
	__atomic_clear(&userspace_heap_state_locked, __ATOMIC_RELEASE);
}

bool heap_grow_region(size_t minimum_size, void** out_base, size_t* out_size) {
	cap_id_t                          memory_cap = CAP_ID_INVALID;
	struct address_space_map_response mapped     = {.mapping_cap = CAP_ID_INVALID};
	size_t                            size;
	if (out_base == NULL || out_size == NULL || minimum_size == 0u) return false;
	*out_base = NULL;
	*out_size = 0u;
	userspace_heap_state_lock();
	if (!runtime_heap_is_configured() || !align_up_size(minimum_size, runtime_heap_granule, &size)) {
		userspace_heap_state_unlock();
		return false;
	}
	if (size <= runtime_heap_size - runtime_heap_used_size) {
		*out_base = (void*)(runtime_heap_base + runtime_heap_used_size);
		*out_size = size;
		runtime_heap_used_size += size;
		userspace_heap_state_unlock();
		return true;
	}
	userspace_heap_state_unlock();
	if (!syscall_status_is_success(memory_allocator_alloc(runtime_heap_memory_allocator_cap, size, &memory_cap)))
		return false;
	if (!syscall_status_is_success(address_space_map(runtime_heap_address_space_cap,
	                                                 memory_cap,
	                                                 MEMORY_ACCESS_READ | MEMORY_ACCESS_WRITE,
	                                                 0u,
	                                                 0u,
	                                                 0u,
	                                                 0u,
	                                                 &mapped)) ||
	    mapped.address == 0u)
		goto cleanup;
	if (!syscall_status_is_success(cap_drop(memory_cap))) goto cleanup;
	memory_cap = CAP_ID_INVALID;
	*out_base  = (void*)mapped.address;
	*out_size  = size;
	return true;
cleanup:
	if (mapped.mapping_cap != CAP_ID_INVALID) (void)mapping_unmap(mapped.mapping_cap);
	if (memory_cap != CAP_ID_INVALID) (void)cap_drop(memory_cap);
	return false;
}

size_t heap_growth_granule(void) {
	return runtime_heap_granule;
}

void heap_lock(void) {
	while (__atomic_test_and_set(&userspace_heap_locked, __ATOMIC_ACQUIRE)) __asm__ volatile("" ::: "memory");
}

void heap_unlock(void) {
	__atomic_clear(&userspace_heap_locked, __ATOMIC_RELEASE);
}
