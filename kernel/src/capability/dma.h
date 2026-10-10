#pragma once

#include <base/cap.h>
#include <base/process.h>
#include <stdbool.h>

/* Publish the singleton DMA control resource when an IOMMU controller exists. */
bool kernel_capability_dma_init(void);

/* Return whether the DMA control resource can currently be acquired. */
bool kernel_capability_dma_available(void);

/* Grant one process root DMA control authority. */
cap_id_t kernel_capability_dma_grant(process_id_t recipient);

#if defined(KERNEL_CAPABILITY_DMA_TEST)
/* Make the next DMA-source claim fail while publishing its response. */
void kernel_capability_dma_test_fail_next_claim_response(void);

/* Return the capability rolled back by the last injected claim-response failure. */
cap_id_t kernel_capability_dma_test_last_rollback_cap(void);
#endif
