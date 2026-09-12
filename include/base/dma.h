#pragma once

#include <stdint.h>

enum dma_sync_target {
	DMA_SYNC_FOR_DEVICE = 0,
	DMA_SYNC_FOR_CPU,
};

typedef uint64_t dma_source_t;

#define DMA_SOURCE_INVALID UINT64_MAX
