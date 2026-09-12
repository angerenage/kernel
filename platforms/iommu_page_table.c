#include "iommu_page_table.h"

#include <base/math.h>
#include <core/mm.h>
#include <hal/hcf.h>
#include <hal/paging.h>
#include <string.h>

#include "paging_transaction.h"

static inline void iommu_pt_publish(void) {
#if defined(PLATFORM_PC_AARCH64)
	__asm__ volatile("dsb oshst" : : : "memory");
#else
	__atomic_thread_fence(__ATOMIC_RELEASE);
#endif
}

void* iommu_pt_phys_to_virt(uintptr_t address) {
	return (void*)(address + boot_info.direct_map_offset);
}

bool iommu_pt_map_mmio(uintptr_t address, size_t size) {
	const struct hal_paging_info* paging = hal_paging_info();
	if (paging == NULL || paging->minimum_leaf_size == 0u || size == 0u) return false;
	size_t    granule = paging->minimum_leaf_size;
	uintptr_t start   = address & ~((uintptr_t)granule - 1u);
	uintptr_t last;
	if (address > UINTPTR_MAX - (size - 1u)) return false;
	last = (address + size - 1u) & ~((uintptr_t)granule - 1u);
	for (uintptr_t physical = start;; physical += granule) {
		if (physical > UINTPTR_MAX - boot_info.direct_map_offset) return false;
		uintptr_t                     virtual_address = physical + boot_info.direct_map_offset;
		struct hal_paging_translation translation;
		if (hal_paging_query(hal_paging_kernel_space(), virtual_address, &translation)) {
			if (translation.physical_address != physical ||
			    (translation.flags & (HAL_PAGE_READ | HAL_PAGE_WRITE)) != (HAL_PAGE_READ | HAL_PAGE_WRITE))
				return false;
#if defined(PLATFORM_PC_RISCV64)
			/* Firmware-described MMIO PMAs remain authoritative without S-mode Svpbmt. */
#else
			if (translation.memory_type != MEMORY_TYPE_DEVICE) return false;
#endif
		}
		else {
			struct hal_paging_map_request request = {.virtual_address  = virtual_address,
			                                         .physical_address = physical,
			                                         .size             = granule,
			                                         .flags       = HAL_PAGE_READ | HAL_PAGE_WRITE | HAL_PAGE_GLOBAL,
			                                         .memory_type = MEMORY_TYPE_DEVICE};
#if defined(PLATFORM_PC_RISCV64)
			/* Firmware-described MMIO has device PMAs even when S-mode cannot prove that Svpbmt is enabled. */
			if (!hal_paging_mapping_supported(request.flags, request.memory_type))
				request.memory_type = MEMORY_TYPE_NORMAL;
#endif
			if (!hal_paging_map(hal_paging_kernel_space(), &request)) return false;
		}
		if (physical == last) break;
	}
	return true;
}

static inline unsigned iommu_pt_page_shift(const struct iommu_pt_format* format) {
	return format->page_shift == 0u ? IOMMU_PT_PAGE_SHIFT : format->page_shift;
}

static inline unsigned iommu_pt_index_bits(const struct iommu_pt_format* format) {
	return format->index_bits == 0u ? IOMMU_PT_INDEX_BITS : format->index_bits;
}

static inline size_t iommu_pt_entry_count(const struct iommu_pt_format* format) {
	return (size_t)1u << iommu_pt_index_bits(format);
}

static inline size_t iommu_pt_leaf_size(const struct iommu_pt_format* format, unsigned level) {
	return (size_t)1u << (iommu_pt_page_shift(format) + iommu_pt_index_bits(format) * level);
}

static inline uint64_t iommu_pt_encode_address(const struct iommu_pt_format* format, uintptr_t address) {
	if (format->kind == IOMMU_PT_RISCV) return ((uint64_t)address >> IOMMU_PT_PAGE_SHIFT) << 10u;
	return (uint64_t)address & format->address_mask;
}

static inline uintptr_t iommu_pt_decode_address(const struct iommu_pt_format* format, uint64_t entry) {
	if (format->kind == IOMMU_PT_RISCV)
		return (uintptr_t)(((entry >> 10u) << IOMMU_PT_PAGE_SHIFT) & format->address_mask);
	return (uintptr_t)(entry & format->address_mask);
}

static inline bool iommu_pt_entry_present(const struct iommu_pt_format* format, uint64_t entry) {
	switch (format->kind) {
	case IOMMU_PT_INTEL_VTD:
		return (entry & 0x3u) != 0u;
	case IOMMU_PT_AMD_V1:
		return (entry & 0x1u) != 0u;
	case IOMMU_PT_ARM_STAGE2:
	case IOMMU_PT_RISCV:
	case IOMMU_PT_LOONGARCH_V1:
		return (entry & 0x1u) != 0u;
	}
	return false;
}

static inline bool iommu_pt_entry_leaf(const struct iommu_pt_format* format, uint64_t entry, unsigned level) {
	if (!iommu_pt_entry_present(format, entry)) return false;
	if (level == 0u) return true;
	switch (format->kind) {
	case IOMMU_PT_INTEL_VTD:
		return (entry & (1ull << 7u)) != 0u;
	case IOMMU_PT_AMD_V1:
		return ((entry >> 9u) & 7u) == 0u;
	case IOMMU_PT_ARM_STAGE2:
		return (entry & 0x2u) == 0u;
	case IOMMU_PT_RISCV:
		return (entry & 0xeu) != 0u;
	case IOMMU_PT_LOONGARCH_V1:
		return (entry & (1ull << 1u)) != 0u;
	}
	return false;
}

static inline uint64_t iommu_pt_table_entry(const struct iommu_pt_format* format, uintptr_t address,
                                            unsigned remaining_levels) {
	uint64_t encoded = iommu_pt_encode_address(format, address);
	switch (format->kind) {
	case IOMMU_PT_INTEL_VTD:
		return encoded | 0x3u;
	case IOMMU_PT_AMD_V1:
		return encoded | 1u | (1ull << 60u) | (1ull << 61u) | (1ull << 62u) | ((uint64_t)remaining_levels << 9u);
	case IOMMU_PT_ARM_STAGE2:
		return encoded | 0x3u;
	case IOMMU_PT_RISCV:
		return encoded | 0x1u;
	case IOMMU_PT_LOONGARCH_V1:
		return encoded | 0x1u;
	}
	return 0u;
}

static inline uint64_t iommu_pt_leaf_entry(const struct iommu_pt_format* format, uintptr_t address, unsigned level,
                                           uint64_t access) {
	uint64_t entry = iommu_pt_encode_address(format, address);
	switch (format->kind) {
	case IOMMU_PT_INTEL_VTD:
		entry |= access & HAL_IOMMU_READ ? 1u : 0u;
		entry |= access & HAL_IOMMU_WRITE ? 2u : 0u;
		if (level != 0u) entry |= 1ull << 7u;
		break;
	case IOMMU_PT_AMD_V1:
		entry |= 1u | (1ull << 60u);
		if ((access & HAL_IOMMU_READ) != 0u) entry |= 1ull << 61u;
		if ((access & HAL_IOMMU_WRITE) != 0u) entry |= 1ull << 62u;
		break;
	case IOMMU_PT_ARM_STAGE2: {
		uint64_t s2ap = 0u;
		if ((access & HAL_IOMMU_READ) != 0u) s2ap |= 1u;
		if ((access & HAL_IOMMU_WRITE) != 0u) s2ap |= 2u;
		entry |= 1u | (level == 0u ? 2u : 0u) | (0xfull << 2u) | (s2ap << 6u) | (3ull << 8u) | (1ull << 10u) |
		         (1ull << 53u) | (1ull << 54u);
		break;
	}
	case IOMMU_PT_RISCV:
		entry |= 1u | (1ull << 4u) | (1ull << 6u) | (1ull << 7u);
		if ((access & HAL_IOMMU_READ) != 0u) entry |= 1ull << 1u;
		if ((access & HAL_IOMMU_WRITE) != 0u) entry |= 1ull << 2u;
		break;
	case IOMMU_PT_LOONGARCH_V1:
		entry |= 1u;
		if (level != 0u) entry |= 1ull << 1u;
		if ((access & HAL_IOMMU_READ) != 0u) entry |= 1ull << 2u;
		if ((access & HAL_IOMMU_WRITE) != 0u) entry |= 1ull << 3u;
		break;
	}
	return entry;
}

static inline uint64_t iommu_pt_leaf_access(const struct iommu_pt_format* format, uint64_t entry) {
	uint64_t access = 0u;
	switch (format->kind) {
	case IOMMU_PT_INTEL_VTD:
		if ((entry & 1u) != 0u) access |= HAL_IOMMU_READ;
		if ((entry & 2u) != 0u) access |= HAL_IOMMU_WRITE;
		break;
	case IOMMU_PT_AMD_V1:
		if ((entry & (1ull << 61u)) != 0u) access |= HAL_IOMMU_READ;
		if ((entry & (1ull << 62u)) != 0u) access |= HAL_IOMMU_WRITE;
		break;
	case IOMMU_PT_ARM_STAGE2:
		if ((entry & (1ull << 6u)) != 0u) access |= HAL_IOMMU_READ;
		if ((entry & (1ull << 7u)) != 0u) access |= HAL_IOMMU_WRITE;
		break;
	case IOMMU_PT_RISCV:
		if ((entry & (1ull << 1u)) != 0u) access |= HAL_IOMMU_READ;
		if ((entry & (1ull << 2u)) != 0u) access |= HAL_IOMMU_WRITE;
		break;
	case IOMMU_PT_LOONGARCH_V1:
		if ((entry & (1ull << 2u)) != 0u) access |= HAL_IOMMU_READ;
		if ((entry & (1ull << 3u)) != 0u) access |= HAL_IOMMU_WRITE;
		break;
	}
	return access;
}

bool iommu_pt_access_supported(const struct iommu_pt_format* format, uint64_t access) {
	if ((access & ~HAL_IOMMU_ACCESS_VALID_MASK) != 0u || access == 0u) return false;
	return format->kind != IOMMU_PT_RISCV || access != HAL_IOMMU_WRITE;
}

bool iommu_pt_allocate_table(size_t allocation_size, struct pmm_extent* out) {
	if (!pmm_alloc(&(const struct pmm_alloc_request){.size = allocation_size, .alignment = allocation_size}, out))
		return false;
	memset(iommu_pt_phys_to_virt(out->address), 0, allocation_size);
	return true;
}

bool iommu_pt_space_init(const struct iommu_pt_format* format, uintptr_t controller_identity, uint32_t context_id,
                         struct hal_iommu_space_state* space) {
	const struct pmm_info* pmm = pmm_info();
	struct pmm_extent      root;
	size_t                 allocation_size;

	if (format == NULL || space == NULL || pmm == NULL || pmm->allocation_granule == 0u ||
	    (pmm->allocation_granule & (pmm->allocation_granule - 1u)) != 0u)
		return false;
	size_t table_size = (size_t)1u << iommu_pt_page_shift(format);
	allocation_size   = pmm->allocation_granule > table_size ? pmm->allocation_granule : table_size;
	if (!iommu_pt_allocate_table(allocation_size, &root)) return false;
	if (((uint64_t)root.address & ~format->address_mask) != 0u) {
		(void)pmm_free(root);
		return false;
	}
	*space = (struct hal_iommu_space_state){
		.table = {.initialized           = true,
	              .root_address          = root.address,
	              .table_allocation_size = allocation_size,
	              .controller_identity   = controller_identity,
	              .context_id            = context_id,
	              .levels                = format->levels}
    };
	return true;
}

static inline uint64_t* iommu_pt_root(const struct hal_iommu_space_state* space) {
	return space == NULL || !space->table.initialized ? NULL
	                                                  : (uint64_t*)iommu_pt_phys_to_virt(space->table.root_address);
}

static inline bool iommu_pt_query(const struct iommu_pt_format* format, const struct hal_iommu_space_state* space,
                                  uint64_t io_address, uint64_t* out_entry, unsigned* out_level) {
	uint64_t* table = iommu_pt_root(space);
	if (table == NULL) return false;
	for (unsigned level = format->levels; level != 0u;) {
		level--;
		uint64_t entry = table[(io_address >> (iommu_pt_page_shift(format) + iommu_pt_index_bits(format) * level)) &
		                       (iommu_pt_entry_count(format) - 1u)];
		if (!iommu_pt_entry_present(format, entry)) return false;
		if (iommu_pt_entry_leaf(format, entry, level)) {
			if (out_entry != NULL) *out_entry = entry;
			if (out_level != NULL) *out_level = level;
			return true;
		}
		table = (uint64_t*)iommu_pt_phys_to_virt(iommu_pt_decode_address(format, entry));
	}
	return false;
}

static inline bool iommu_pt_range_mapped(const struct iommu_pt_format*       format,
                                         const struct hal_iommu_space_state* space, uint64_t start, uint64_t end) {
	while (start < end) {
		unsigned level;
		if (!iommu_pt_query(format, space, start, NULL, &level)) return false;
		size_t   leaf_size = iommu_pt_leaf_size(format, level);
		uint64_t leaf_end  = (start & ~((uint64_t)leaf_size - 1u)) + leaf_size;
		start              = leaf_end < end ? leaf_end : end;
	}
	return true;
}

static inline bool iommu_pt_table_range_unmapped(const struct iommu_pt_format* format, const uint64_t* table,
                                                 unsigned level, uint64_t start, uint64_t end) {
	size_t span = iommu_pt_leaf_size(format, level);
	while (start < end) {
		size_t index = (start >> (iommu_pt_page_shift(format) + iommu_pt_index_bits(format) * level)) &
		               (iommu_pt_entry_count(format) - 1u);
		uint64_t entry     = table[index];
		uint64_t entry_end = (start & ~((uint64_t)span - 1u)) + span;
		uint64_t next      = entry_end < end ? entry_end : end;
		if (iommu_pt_entry_present(format, entry)) {
			if (iommu_pt_entry_leaf(format, entry, level) || level == 0u) return false;
			if (!iommu_pt_table_range_unmapped(
					format,
					(const uint64_t*)iommu_pt_phys_to_virt(iommu_pt_decode_address(format, entry)),
					level - 1u,
					start,
					next))
				return false;
		}
		start = next;
	}
	return true;
}

static inline bool iommu_pt_range_unmapped(const struct iommu_pt_format*       format,
                                           const struct hal_iommu_space_state* space, uint64_t start, uint64_t end) {
	const uint64_t* root = iommu_pt_root(space);
	return root != NULL && iommu_pt_table_range_unmapped(format, root, format->levels - 1u, start, end);
}

struct iommu_pt_restore_context {
	const struct iommu_pt_format* format;
	iommu_pt_sync_fn              sync;
	void*                         sync_context;
	uint32_t                      context_id;
};

static inline void iommu_pt_restore(uint64_t* slot, uint64_t previous, void* raw_context) {
	struct iommu_pt_restore_context* context = raw_context;
	if (context->format->kind == IOMMU_PT_ARM_STAGE2 && iommu_pt_entry_present(context->format, previous) &&
	    *slot != previous) {
		*slot = 0u;
		iommu_pt_publish();
		if (context->sync != NULL && !context->sync(context->sync_context, context->context_id, 0u, 0u, true)) hcf();
	}
	*slot = previous;
	if (context->format->kind == IOMMU_PT_ARM_STAGE2) iommu_pt_publish();
}

static inline bool iommu_pt_walk(const struct iommu_pt_format* format, struct hal_iommu_space_state* space,
                                 uint64_t io_address, unsigned target_level, struct paging_transaction* transaction,
                                 uint64_t** out_slot) {
	uint64_t* table = iommu_pt_root(space);
	if (table == NULL || target_level >= format->levels) return false;
	for (unsigned level = format->levels - 1u; level > target_level; level--) {
		size_t index = (io_address >> (iommu_pt_page_shift(format) + iommu_pt_index_bits(format) * level)) &
		               (iommu_pt_entry_count(format) - 1u);
		uint64_t entry = table[index];
		if (!iommu_pt_entry_present(format, entry)) {
			struct pmm_extent allocation;
			if (!iommu_pt_allocate_table(space->table.table_allocation_size, &allocation)) return false;
			if (((uint64_t)allocation.address & ~format->address_mask) != 0u) {
				(void)pmm_free(allocation);
				return false;
			}
			if (!paging_transaction_record(transaction, &table[index], allocation)) {
				(void)pmm_free(allocation);
				return false;
			}
			table[index] = iommu_pt_table_entry(format, allocation.address, level);
			entry        = table[index];
		}
		else if (iommu_pt_entry_leaf(format, entry, level)) {
			return false;
		}
		table = (uint64_t*)iommu_pt_phys_to_virt(iommu_pt_decode_address(format, entry));
	}
	*out_slot = &table[(io_address >> (iommu_pt_page_shift(format) + iommu_pt_index_bits(format) * target_level)) &
	                   (iommu_pt_entry_count(format) - 1u)];
	return true;
}

static inline unsigned iommu_pt_choose_level(const struct iommu_pt_format* format, uint64_t io_address,
                                             uintptr_t physical_address, size_t remaining) {
	for (unsigned level = format->levels; level != 0u;) {
		level--;
		size_t size = iommu_pt_leaf_size(format, level);
		if ((format->leaf_size_mask & (1ull << (iommu_pt_page_shift(format) + iommu_pt_index_bits(format) * level))) !=
		        0u &&
		    (io_address & (size - 1u)) == 0u && (physical_address & (size - 1u)) == 0u && remaining >= size)
			return level;
	}
	return 0u;
}

static inline bool iommu_pt_args(const struct iommu_pt_format* format, uint64_t io_address, uintptr_t physical_address,
                                 size_t size, bool check_physical, uint64_t* out_end) {
	uint64_t end;
	uint64_t physical_end;
	uint64_t io_limit;
	uint64_t pa_limit;
	if (format->io_address_bits == 0u || format->io_address_bits > 64u || format->physical_address_bits == 0u ||
	    format->physical_address_bits > 64u)
		return false;
	io_limit = format->io_address_bits == 64u ? UINT64_MAX : 1ull << format->io_address_bits;
	pa_limit = format->physical_address_bits == 64u ? UINT64_MAX : 1ull << format->physical_address_bits;
	if (size == 0u || (io_address & (((size_t)1u << iommu_pt_page_shift(format)) - 1u)) != 0u ||
	    (size & (((size_t)1u << iommu_pt_page_shift(format)) - 1u)) != 0u || add_overflow_u64(io_address, size, &end) ||
	    (format->io_address_bits != 64u && end > io_limit))
		return false;
	if (check_physical && ((physical_address & (((size_t)1u << iommu_pt_page_shift(format)) - 1u)) != 0u ||
	                       add_overflow_u64((uint64_t)physical_address, size, &physical_end) ||
	                       (format->physical_address_bits != 64u && physical_end > pa_limit)))
		return false;
	*out_end = end;
	return true;
}

bool iommu_pt_map(const struct iommu_pt_format* format, struct hal_iommu_space_state* space,
                  const struct hal_iommu_map_request* request, iommu_pt_sync_fn sync, void* context) {
	struct paging_transaction       transaction = {0};
	uint64_t                        end;
	uint64_t                        io;
	uintptr_t                       physical;
	struct iommu_pt_restore_context restore = {format, sync, context, space == NULL ? 0u : space->table.context_id};
	if (format == NULL || space == NULL || request == NULL || !iommu_pt_access_supported(format, request->access) ||
	    !iommu_pt_args(format, request->io_address, request->physical_address, request->size, true, &end) ||
	    !iommu_pt_range_unmapped(format, space, request->io_address, end))
		return false;
	io       = request->io_address;
	physical = request->physical_address;
	while (io < end) {
		unsigned  level     = iommu_pt_choose_level(format, io, physical, (size_t)(end - io));
		size_t    leaf_size = iommu_pt_leaf_size(format, level);
		uint64_t* slot;
		if (!iommu_pt_walk(format, space, io, level, &transaction, &slot) || *slot != 0u ||
		    !paging_transaction_record(&transaction, slot, (struct pmm_extent){0}))
			goto rollback;
		*slot = iommu_pt_leaf_entry(format, physical, level, request->access);
		io += leaf_size;
		physical += leaf_size;
	}
	iommu_pt_publish();
	if (sync != NULL &&
	    !sync(context, space->table.context_id, request->io_address, request->size, transaction.hierarchy_changed))
		hcf();
	paging_transaction_commit(&transaction);
	space->table.mapped_size += request->size;
	return true;

rollback:
	if (paging_transaction_empty(&transaction)) return false;
	paging_transaction_rollback(&transaction, iommu_pt_restore, &restore);
	iommu_pt_publish();
	if (sync != NULL && !sync(context, space->table.context_id, request->io_address, request->size, true)) hcf();
	paging_transaction_abort(&transaction);
	return false;
}

static inline bool iommu_pt_split_leaf(const struct iommu_pt_format* format, struct hal_iommu_space_state* space,
                                       uint64_t* slot, unsigned level, uint64_t io_address,
                                       struct paging_transaction* transaction, iommu_pt_sync_fn sync, void* context) {
	uint64_t          entry = *slot;
	struct pmm_extent allocation;
	uint64_t*         child;
	uintptr_t         base;
	size_t            child_size;
	uint64_t          access;
	if (level == 0u || !iommu_pt_entry_leaf(format, entry, level) ||
	    !iommu_pt_allocate_table(space->table.table_allocation_size, &allocation))
		return false;
	if (((uint64_t)allocation.address & ~format->address_mask) != 0u) {
		(void)pmm_free(allocation);
		return false;
	}
	child      = (uint64_t*)iommu_pt_phys_to_virt(allocation.address);
	child_size = iommu_pt_leaf_size(format, level - 1u);
	base       = iommu_pt_decode_address(format, entry) & ~((uintptr_t)iommu_pt_leaf_size(format, level) - 1u);
	access     = iommu_pt_leaf_access(format, entry);
	for (size_t index = 0u; index < iommu_pt_entry_count(format); index++)
		child[index] = iommu_pt_leaf_entry(format, base + index * child_size, level - 1u, access);
	if (!paging_transaction_record(transaction, slot, allocation)) {
		(void)pmm_free(allocation);
		return false;
	}
	if (format->kind == IOMMU_PT_ARM_STAGE2) {
		*slot = 0u;
		iommu_pt_publish();
		if (sync != NULL && !sync(context,
		                          space->table.context_id,
		                          io_address & ~((uint64_t)iommu_pt_leaf_size(format, level) - 1u),
		                          iommu_pt_leaf_size(format, level),
		                          true))
			hcf();
	}
	*slot = iommu_pt_table_entry(format, allocation.address, level);
	return true;
}

static inline bool iommu_pt_protect_range(const struct iommu_pt_format* format, struct hal_iommu_space_state* space,
                                          uint64_t* table, unsigned level, uint64_t start, uint64_t end,
                                          uint64_t access, struct paging_transaction* transaction,
                                          iommu_pt_sync_fn sync, void* context, bool* out_changed) {
	size_t span = iommu_pt_leaf_size(format, level);
	while (start < end) {
		size_t index = (start >> (iommu_pt_page_shift(format) + iommu_pt_index_bits(format) * level)) &
		               (iommu_pt_entry_count(format) - 1u);
		uint64_t  leaf_start = start & ~((uint64_t)span - 1u);
		uint64_t  leaf_end   = leaf_start + span;
		uint64_t  next       = leaf_end < end ? leaf_end : end;
		uint64_t* slot       = &table[index];
		uint64_t  entry      = *slot;
		if (!iommu_pt_entry_present(format, entry)) return false;
		if (iommu_pt_entry_leaf(format, entry, level)) {
			if (level != 0u && (start != leaf_start || next != leaf_end) &&
			    !iommu_pt_split_leaf(format, space, slot, level, start, transaction, sync, context))
				return false;
			entry = *slot;
		}
		if (iommu_pt_entry_leaf(format, entry, level)) {
			uint64_t new_entry = iommu_pt_leaf_entry(format, iommu_pt_decode_address(format, entry), level, access);
			if (entry != new_entry) {
				if (!paging_transaction_record(transaction, slot, (struct pmm_extent){0})) return false;
				if (format->kind == IOMMU_PT_ARM_STAGE2) {
					*slot = 0u;
					iommu_pt_publish();
					if (sync != NULL && !sync(context, space->table.context_id, start, (size_t)(next - start), true))
						hcf();
				}
				*slot = new_entry;
				if (format->kind == IOMMU_PT_ARM_STAGE2) {
					iommu_pt_publish();
					if (sync != NULL && !sync(context, space->table.context_id, start, (size_t)(next - start), true))
						hcf();
				}
				if (out_changed != NULL) *out_changed = true;
			}
		}
		else {
			if (!iommu_pt_protect_range(format,
			                            space,
			                            (uint64_t*)iommu_pt_phys_to_virt(iommu_pt_decode_address(format, entry)),
			                            level - 1u,
			                            start,
			                            next,
			                            access,
			                            transaction,
			                            sync,
			                            context,
			                            out_changed))
				return false;
		}
		start = next;
	}
	return true;
}

bool iommu_pt_protect(const struct iommu_pt_format* format, struct hal_iommu_space_state* space, uint64_t io_address,
                      size_t size, uint64_t access, iommu_pt_sync_fn sync, void* context) {
	struct paging_transaction       transaction = {0};
	uint64_t                        end;
	uint64_t*                       root;
	struct iommu_pt_restore_context restore = {format, sync, context, space == NULL ? 0u : space->table.context_id};
	bool                            changed = false;
	if (format == NULL || space == NULL || !iommu_pt_access_supported(format, access) ||
	    !iommu_pt_args(format, io_address, 0u, size, false, &end) ||
	    !iommu_pt_range_mapped(format, space, io_address, end))
		return false;
	root = iommu_pt_root(space);
	if (root == NULL ||
	    !iommu_pt_protect_range(
			format, space, root, format->levels - 1u, io_address, end, access, &transaction, sync, context, &changed))
		goto rollback;
	if (changed) {
		iommu_pt_publish();
		if (sync != NULL && !sync(context, space->table.context_id, io_address, size, transaction.hierarchy_changed))
			hcf();
	}
	paging_transaction_commit(&transaction);
	return true;

rollback:
	if (paging_transaction_empty(&transaction)) return false;
	paging_transaction_rollback(&transaction, iommu_pt_restore, &restore);
	iommu_pt_publish();
	if (sync != NULL && !sync(context, space->table.context_id, io_address, size, true)) hcf();
	paging_transaction_abort(&transaction);
	return false;
}

static inline bool iommu_pt_prepare_unmap(const struct iommu_pt_format* format, struct hal_iommu_space_state* space,
                                          uint64_t* table, unsigned level, uint64_t start, uint64_t end,
                                          struct paging_transaction* transaction, iommu_pt_sync_fn sync,
                                          void* context) {
	size_t span = iommu_pt_leaf_size(format, level);
	while (start < end) {
		size_t index = (start >> (iommu_pt_page_shift(format) + iommu_pt_index_bits(format) * level)) &
		               (iommu_pt_entry_count(format) - 1u);
		uint64_t  leaf_start = start & ~((uint64_t)span - 1u);
		uint64_t  leaf_end   = leaf_start + span;
		uint64_t  next       = leaf_end < end ? leaf_end : end;
		uint64_t* slot       = &table[index];
		uint64_t  entry      = *slot;
		if (!iommu_pt_entry_present(format, entry)) return false;
		if (iommu_pt_entry_leaf(format, entry, level)) {
			if (level != 0u && (start != leaf_start || next != leaf_end) &&
			    !iommu_pt_split_leaf(format, space, slot, level, start, transaction, sync, context))
				return false;
			entry = *slot;
		}
		if (!iommu_pt_entry_leaf(format, entry, level) &&
		    !iommu_pt_prepare_unmap(format,
		                            space,
		                            (uint64_t*)iommu_pt_phys_to_virt(iommu_pt_decode_address(format, entry)),
		                            level - 1u,
		                            start,
		                            next,
		                            transaction,
		                            sync,
		                            context))
			return false;
		start = next;
	}
	return true;
}

static inline bool iommu_pt_table_empty(const struct iommu_pt_format* format, const uint64_t* table) {
	for (size_t index = 0u; index < iommu_pt_entry_count(format); index++)
		if (iommu_pt_entry_present(format, table[index])) return false;
	return true;
}

static inline bool iommu_pt_remove_range(const struct iommu_pt_format* format, struct hal_iommu_space_state* space,
                                         uint64_t* table, unsigned level, uint64_t start, uint64_t end,
                                         struct paging_transaction* transaction) {
	size_t span = iommu_pt_leaf_size(format, level);
	while (start < end) {
		size_t index = (start >> (iommu_pt_page_shift(format) + iommu_pt_index_bits(format) * level)) &
		               (iommu_pt_entry_count(format) - 1u);
		uint64_t leaf_start = start & ~((uint64_t)span - 1u);
		uint64_t leaf_end   = leaf_start + span;
		uint64_t next       = leaf_end < end ? leaf_end : end;
		uint64_t entry      = table[index];
		if (iommu_pt_entry_leaf(format, entry, level)) {
			if (!paging_transaction_record(transaction, &table[index], (struct pmm_extent){0})) return false;
			table[index] = 0u;
		}
		else {
			uintptr_t child_address = iommu_pt_decode_address(format, entry);
			uint64_t* child         = (uint64_t*)iommu_pt_phys_to_virt(child_address);
			if (!iommu_pt_remove_range(format, space, child, level - 1u, start, next, transaction)) return false;
			if (iommu_pt_table_empty(format, child)) {
				if (!paging_transaction_retire(
						transaction,
						&table[index],
						(struct pmm_extent){.address = child_address, .size = space->table.table_allocation_size}))
					return false;
				table[index] = 0u;
			}
		}
		start = next;
	}
	return true;
}

bool iommu_pt_unmap(const struct iommu_pt_format* format, struct hal_iommu_space_state* space, uint64_t io_address,
                    size_t size, iommu_pt_sync_fn sync, void* context) {
	struct paging_transaction       transaction = {0};
	uint64_t                        end;
	uint64_t*                       root;
	struct iommu_pt_restore_context restore = {format, sync, context, space == NULL ? 0u : space->table.context_id};
	if (format == NULL || space == NULL || !iommu_pt_args(format, io_address, 0u, size, false, &end) ||
	    !iommu_pt_range_mapped(format, space, io_address, end))
		return false;
	root = iommu_pt_root(space);
	if (root == NULL ||
	    !iommu_pt_prepare_unmap(
			format, space, root, format->levels - 1u, io_address, end, &transaction, sync, context) ||
	    !iommu_pt_remove_range(format, space, root, format->levels - 1u, io_address, end, &transaction))
		goto rollback;
	iommu_pt_publish();
	if (sync != NULL && !sync(context, space->table.context_id, io_address, size, transaction.hierarchy_changed)) hcf();
	paging_transaction_commit(&transaction);
	space->table.mapped_size -= size;
	return true;

rollback:
	if (paging_transaction_empty(&transaction)) return false;
	paging_transaction_rollback(&transaction, iommu_pt_restore, &restore);
	iommu_pt_publish();
	if (sync != NULL && !sync(context, space->table.context_id, io_address, size, true)) hcf();
	paging_transaction_abort(&transaction);
	return false;
}

static inline void iommu_pt_free_children(const struct iommu_pt_format*       format,
                                          const struct hal_iommu_space_state* space, uint64_t* table, unsigned level) {
	if (level == 0u) return;
	for (size_t index = 0u; index < iommu_pt_entry_count(format); index++) {
		uint64_t entry = table[index];
		if (!iommu_pt_entry_present(format, entry) || iommu_pt_entry_leaf(format, entry, level)) continue;
		uintptr_t child_address = iommu_pt_decode_address(format, entry);
		uint64_t* child         = (uint64_t*)iommu_pt_phys_to_virt(child_address);
		iommu_pt_free_children(format, space, child, level - 1u);
		(void)pmm_free((struct pmm_extent){.address = child_address, .size = space->table.table_allocation_size});
	}
}

void iommu_pt_space_deinit(const struct iommu_pt_format* format, struct hal_iommu_space_state* space) {
	uint64_t* root = iommu_pt_root(space);
	if (format == NULL || root == NULL || space->table.mapped_size != 0u) return;
	iommu_pt_free_children(format, space, root, format->levels - 1u);
	(void)pmm_free(
		(struct pmm_extent){.address = space->table.root_address, .size = space->table.table_allocation_size});
	*space = (struct hal_iommu_space_state){0};
}
