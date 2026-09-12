#include <core/mm.h>
#include <core/pmm.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DMA_TEST_HEAP_SIZE (2u * 1024u * 1024u)
#define DMA_TEST_PMM_SIZE (4u * 1024u * 1024u)

static uint8_t dma_test_heap[DMA_TEST_HEAP_SIZE] __attribute__((aligned(4096)));
static uint8_t dma_test_pmm[DMA_TEST_PMM_SIZE] __attribute__((aligned(4096)));
static size_t  dma_test_heap_offset;

bool heap_grow_region(size_t minimum_size, void** out_base, size_t* out_size) {
	if (out_base == NULL || out_size == NULL || minimum_size > DMA_TEST_HEAP_SIZE - dma_test_heap_offset) return false;

	*out_base = dma_test_heap + dma_test_heap_offset;
	*out_size = minimum_size;
	dma_test_heap_offset += minimum_size;
	return true;
}

bool dma_test_init_pmm(void) {
	static bool            initialized;
	const struct mem_range memory_map = {
		.base   = (uintptr_t)dma_test_pmm,
		.length = DMA_TEST_PMM_SIZE,
		.type   = MEM_RANGE_USABLE,
	};

	if (initialized) return true;
	initialized = pmm_init(&memory_map, 1u, 0u);
	return initialized;
}
