#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct address_space;

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
 * Creates a tracked virtual allocation in an explicit address space.
 * - With VMM_MAP_LAZY: reserve only, state starts RESERVED.
 * - Without VMM_MAP_LAZY: pages are allocated + mapped immediately.
 * - Lazy allocations may move through RESERVED -> PARTIAL -> MAPPED.
 * - STACK allocations reserve extra guard pages below the returned base.
 */
bool vmm_alloc(struct address_space* space, const struct vmm_alloc_params* params, vmm_id_t* out_id, void** out_base);

/* Create a tracked virtual allocation whose usable base is exactly base. */
bool vmm_alloc_at(struct address_space* space, void* base, const struct vmm_alloc_params* params, vmm_id_t* out_id);

/* Destroy allocation metadata and backing owned by an explicit address space. */
bool vmm_free(struct address_space* space, vmm_id_t id);

/* Destroy an allocation by base address inside an explicit address space. */
bool vmm_free_at(struct address_space* space, void* base);

/* Materialize all pages in an explicit address-space allocation. */
bool vmm_map(struct address_space* space, vmm_id_t id);

/* Remove all live mappings from an explicit address-space allocation. */
bool vmm_unmap(struct address_space* space, vmm_id_t id, bool release_phys);

/* Change protection flags on an explicit address-space allocation. */
bool vmm_protect(struct address_space* space, vmm_id_t id, vmm_prot_t new_prot);

/* Resolve a lazy fault inside an explicit address space. */
bool vmm_resolve_page_fault(struct address_space* space, uintptr_t addr);

/* Resolve a lazy fault against the current address space. */
bool vmm_resolve_current_page_fault(uintptr_t addr);

/* Query the tracked allocation owning an address inside an explicit address space. */
bool vmm_query(struct address_space* space, void* addr, struct vmm_info* out_info);

/* Query an explicit address-space allocation by id. */
bool vmm_query_id(struct address_space* space, vmm_id_t id, struct vmm_info* out_info);

/* Base virtual address of the managed VMM window. */
uintptr_t vmm_window_base(void);

/* Size of the managed VMM window in pages. */
size_t vmm_window_page_count(void);

/* Number of live tracked allocations in an explicit address space. */
size_t vmm_count(struct address_space* space);

/* Initialize a user address space over the configured user virtual range. */
bool vmm_user_address_space_init(struct address_space* space);

/* Release all VMM metadata/backing and the virtual-range allocator for an address space. */
void vmm_address_space_deinit(struct address_space* space);
