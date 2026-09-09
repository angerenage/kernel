#include <base/heap.h>
#include <runtime/heap.h>
#include <system/memory.h>
#include <system/process.h>

uintptr_t runtime_heap_base;
size_t    runtime_heap_size;
size_t    runtime_heap_used_size;
size_t    runtime_heap_granule;
cap_id_t  runtime_heap_address_space_cap    = CAP_ID_INVALID;
cap_id_t  runtime_heap_memory_allocator_cap = CAP_ID_INVALID;
bool      runtime_heap_configured;

bool runtime_heap_init(const struct process_startup_info* startup) {
	struct self_info          self;
	struct address_space_info address_space;
	if (startup == NULL || startup->heap_base == 0u || startup->heap_size == 0u ||
	    startup->memory_allocator_cap == CAP_ID_INVALID || !syscall_status_is_success(process_self_info(&self)) ||
	    !syscall_status_is_success(address_space_info(self.address_space_cap, &address_space)) ||
	    address_space.kind != ADDRESS_SPACE_KIND_PROCESS || address_space.minimum_mapping_size == 0u ||
	    (address_space.minimum_mapping_size & (address_space.minimum_mapping_size - 1u)) != 0u ||
	    (startup->heap_base & (address_space.minimum_mapping_size - 1u)) != 0u ||
	    (startup->heap_size & (address_space.minimum_mapping_size - 1u)) != 0u || runtime_heap_configured)
		return false;
	runtime_heap_base                 = startup->heap_base;
	runtime_heap_size                 = startup->heap_size;
	runtime_heap_used_size            = 0u;
	runtime_heap_granule              = address_space.minimum_mapping_size;
	runtime_heap_address_space_cap    = self.address_space_cap;
	runtime_heap_memory_allocator_cap = startup->memory_allocator_cap;
	runtime_heap_configured           = true;
	return heap_init();
}

bool runtime_heap_is_configured(void) {
	return runtime_heap_configured;
}
