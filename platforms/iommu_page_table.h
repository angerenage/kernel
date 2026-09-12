#pragma once

#include <core/pmm.h>
#include <hal/iommu.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define IOMMU_PT_PAGE_SHIFT 12u
#define IOMMU_PT_PAGE_SIZE ((size_t)1u << IOMMU_PT_PAGE_SHIFT)
#define IOMMU_PT_INDEX_BITS 9u

enum iommu_pt_kind {
	IOMMU_PT_INTEL_VTD = 0,
	IOMMU_PT_AMD_V1,
	IOMMU_PT_ARM_STAGE2,
	IOMMU_PT_RISCV,
	IOMMU_PT_LOONGARCH_V1,
};

struct iommu_pt_format {
	enum iommu_pt_kind kind;
	uint64_t           address_mask;
	uint64_t           leaf_size_mask;
	uint8_t            levels;
	uint8_t            io_address_bits;
	uint8_t            physical_address_bits;
	uint8_t            page_shift;
	uint8_t            index_bits;
};

typedef bool (*iommu_pt_sync_fn)(void* context, uint32_t context_id, uint64_t io_address, size_t size,
                                 bool hierarchy_changed);

void* iommu_pt_phys_to_virt(uintptr_t address);
bool  iommu_pt_map_mmio(uintptr_t address, size_t size);
bool  iommu_pt_access_supported(const struct iommu_pt_format* format, uint64_t access);
bool  iommu_pt_allocate_table(size_t allocation_size, struct pmm_extent* out);
bool  iommu_pt_space_init(const struct iommu_pt_format* format, uintptr_t controller_identity, uint32_t context_id,
                          struct hal_iommu_space_state* space);
bool  iommu_pt_map(const struct iommu_pt_format* format, struct hal_iommu_space_state* space,
                   const struct hal_iommu_map_request* request, iommu_pt_sync_fn sync, void* context);
bool  iommu_pt_protect(const struct iommu_pt_format* format, struct hal_iommu_space_state* space, uint64_t io_address,
                       size_t size, uint64_t access, iommu_pt_sync_fn sync, void* context);
bool  iommu_pt_unmap(const struct iommu_pt_format* format, struct hal_iommu_space_state* space, uint64_t io_address,
                     size_t size, iommu_pt_sync_fn sync, void* context);
void  iommu_pt_space_deinit(const struct iommu_pt_format* format, struct hal_iommu_space_state* space);
