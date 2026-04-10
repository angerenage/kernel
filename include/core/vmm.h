#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VMM_ID_INVALID 0ull
#define VMM_MIN_ALIGN_PAGES 1u
#define VMM_STACK_DEFAULT_GUARD_PAGES 1u

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
};

#define VMM_PROT_VALID_MASK                                                                                            \
	((vmm_prot_t)(VMM_PROT_WRITE | VMM_PROT_EXEC | VMM_PROT_GLOBAL | VMM_PROT_NO_CACHE | VMM_PROT_READ))

enum vmm_kind {
	VMM_KIND_GENERIC = 0,
	VMM_KIND_HEAP,
	VMM_KIND_STACK,
	VMM_KIND_MMIO,
	VMM_KIND_KERNEL_TEXT,
	VMM_KIND_KERNEL_RODATA,
	VMM_KIND_KERNEL_DATA,
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

/* Initialize the paging backend and reserve the kernel's managed virtual window. Re-initializing resets metadata. */
bool vmm_init(void);

/* Return whether vmm_init() completed successfully. */
bool vmm_is_initialized(void);

/*
 * Creates a tracked virtual allocation.
 * - With VMM_MAP_LAZY: reserve only, state starts RESERVED.
 * - Without VMM_MAP_LAZY: pages are allocated + mapped immediately.
 * - Lazy allocations may move through RESERVED -> PARTIAL -> MAPPED.
 * - STACK allocations reserve extra guard pages below the returned base.
 */
bool vmm_alloc(const struct vmm_alloc_params* params, vmm_id_t* out_id, void** out_base);

/* Destroy allocation metadata and unmap/free any mapped backing pages owned by the allocation. */
bool vmm_free(vmm_id_t id);

/* Same as vmm_free(), but looks the allocation up by its base virtual address. */
bool vmm_free_at(void* base);

/* Materialize all pages in an allocation that is still wholly or partly reserved. */
bool vmm_map(vmm_id_t id);

/* Remove all live mappings from an allocation, optionally releasing its physical backing pages as well. */
bool vmm_unmap(vmm_id_t id, bool release_phys);

/* Change the protection flags on every currently mapped page in an allocation. */
bool vmm_protect(vmm_id_t id, vmm_prot_t new_prot);

/* Resolve a not-present fault by mapping the page that belongs to a lazy reservation, if policy allows it. */
bool vmm_resolve_page_fault(uintptr_t addr);

/* Query the tracked allocation that owns a virtual address. */
bool vmm_query(void* addr, struct vmm_info* out_info);

/* Query an allocation by its stable VMM identifier. */
bool vmm_query_id(vmm_id_t id, struct vmm_info* out_info);

/* Base virtual address of the managed VMM window. */
uintptr_t vmm_window_base(void);

/* Size of the managed VMM window in pages. */
size_t vmm_window_page_count(void);

/* Number of live tracked allocations. */
size_t vmm_count(void);
