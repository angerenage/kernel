#pragma once

#include <stddef.h>
#include <stdint.h>

#define VMM_ID_INVALID 0ull
#define VMM_MIN_ALIGN_PAGES 1u
#define VMM_STACK_DEFAULT_GUARD_PAGES 1u
#define VMM_PAGE_SIZE 0x1000ull

typedef uint64_t vmm_id_t;
typedef uint64_t vmm_prot_t;

/* Protection bits understood by the virtual memory manager. READ is tracked for policy/metadata too. */
enum vmm_prot_flag {
	VMM_PROT_NONE     = 0,
	VMM_PROT_WRITE    = 1u << 0,
	VMM_PROT_EXEC     = 1u << 1,
	VMM_PROT_GLOBAL   = 1u << 2,
	VMM_PROT_NO_CACHE = 1u << 3,
	VMM_PROT_READ     = 1u << 4,
	VMM_PROT_USER     = 1u << 5,
};

#define VMM_PROT_VALID_MASK                                                                                            \
	((vmm_prot_t)(VMM_PROT_WRITE | VMM_PROT_EXEC | VMM_PROT_GLOBAL | VMM_PROT_NO_CACHE | VMM_PROT_READ | VMM_PROT_USER))

enum vmm_kind {
	VMM_KIND_GENERIC = 0,
	VMM_KIND_HEAP,
	VMM_KIND_STACK,
	VMM_KIND_MMIO,
	VMM_KIND_KERNEL_TEXT,
	VMM_KIND_KERNEL_RODATA,
	VMM_KIND_KERNEL_DATA,
	VMM_KIND_PHYSICAL,
};

/* Mapping state of a tracked allocation. */
enum vmm_state {
	VMM_STATE_RESERVED = 0,
	VMM_STATE_PARTIAL,
	VMM_STATE_MAPPED,
};

enum vmm_map_flags {
	VMM_MAP_LAZY = 1u << 0,
};

/* Parameters for one tracked virtual allocation. */
struct vmm_alloc_params {
	/* Number of usable pages to reserve (must be > 0). */
	size_t page_count;
	/* Virtual alignment in pages (power of two, 1 means page aligned). */
	size_t align_pages;
	/* Protection mask composed from VMM_PROT_* symbols only. */
	vmm_prot_t prot;
	/* Logical purpose for diagnostics and policy checks. */
	enum vmm_kind kind;
	/* Stack-only guard area below the usable range; defaults to 1 page when zero. */
	size_t guard_pages;
	/* Optional map policy flags (e.g. lazy map). */
	uint64_t map_flags;
};

/* Public snapshot of one tracked allocation. */
struct vmm_info {
	vmm_id_t       id;
	void*          base;
	size_t         page_count;
	vmm_prot_t     prot;
	enum vmm_kind  kind;
	size_t         guard_pages;
	enum vmm_state state;
	uintptr_t      first_phys;
};
