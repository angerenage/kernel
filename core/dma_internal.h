#pragma once

#include <hal/iommu.h>
#include <stdbool.h>
#include <stdint.h>

/* Return the initialized IOMMU information for a controller. */
const struct hal_iommu_info* dma_controller_info(uint32_t controller_index);

/* Return the initialized HAL state for an IOMMU controller. */
struct hal_iommu_controller_state* dma_controller_state(uint32_t controller_index);

/* Return a DEVICE AddressSpace context ID to its owning controller. */
bool dma_controller_release_context(uint32_t controller_index, uint32_t context_id);
