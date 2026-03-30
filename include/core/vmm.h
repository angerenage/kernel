#pragma once

#include <core/pmm.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum vmm_page_flags {
	VMM_PAGE_WRITE    = 1u << 0,
	VMM_PAGE_EXEC     = 1u << 1,
	VMM_PAGE_GLOBAL   = 1u << 2,
	VMM_PAGE_NO_CACHE = 1u << 3,
};

bool vmm_init(void);
bool vmm_is_initialized(void);

bool vmm_alloc_pages(size_t count, size_t align_pages, uint64_t flags, void** out_virt);
bool vmm_free_pages(void* virt, size_t count);

uintptr_t vmm_heap_base(void);
size_t    vmm_heap_page_count(void);
