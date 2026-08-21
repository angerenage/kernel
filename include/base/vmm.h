#pragma once

#include <stddef.h>
#include <stdint.h>

#define VMM_ID_INVALID 0ull
#define VMM_PAGE_SIZE 0x1000ull
#define VMM_MIN_ALIGN_PAGES 1u
#define VMM_STACK_DEFAULT_GUARD_PAGES 1u

typedef uint64_t vmm_id_t;
typedef uint32_t vmm_prot_t;

enum vmm_prot_flag {
	VMM_PROT_NONE     = 0,
	VMM_PROT_WRITE    = 1u << 0,
	VMM_PROT_EXEC     = 1u << 1,
	VMM_PROT_GLOBAL   = 1u << 2,
	VMM_PROT_NO_CACHE = 1u << 3,
	VMM_PROT_READ     = 1u << 4,
};

#define VMM_PROT_VALID_MASK                                                                                            \
	((vmm_prot_t)(VMM_PROT_WRITE | VMM_PROT_EXEC | VMM_PROT_GLOBAL | VMM_PROT_NO_CACHE | VMM_PROT_READ))

/* Snapshot of one live virtual-memory mapping. */
struct vmm_info {
	vmm_id_t   id;
	void*      base;
	size_t     page_count;
	size_t     memory_page_offset;
	size_t     guard_pages;
	vmm_prot_t prot;
};
