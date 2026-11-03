#include "ring.h"

#include <base/dma.h>
#include <base/memory.h>
#include <string.h>
#include <system/memory.h>

#include "registers.h"

_Static_assert(sizeof(struct xhci_trb) == 16u, "xHCI TRB size");

bool xhci_ring_allocate(const struct xhci_dma_context* dma, struct xhci_ring* ring, bool command) {
	*ring = (struct xhci_ring){
		.dma = {.memory_cap = CAP_ID_INVALID, .cpu_mapping_cap = CAP_ID_INVALID, .device_mapping_cap = CAP_ID_INVALID},
		.producer_cycle = 1u,
		.consumer_cycle = 1u
    };
	if (!xhci_dma_buffer_allocate(
			dma, XHCI_RING_TRBS * sizeof(struct xhci_trb), MEMORY_ACCESS_READ | MEMORY_ACCESS_WRITE, &ring->dma))
		return false;
	if ((ring->dma.device_address & 63u) != 0u) goto fail;
	if (command) {
		struct xhci_trb* trbs    = ring->dma.cpu_address;
		uint64_t         address = (uint64_t)ring->dma.device_address;
		trbs[XHCI_RING_TRBS - 1u] =
			(struct xhci_trb){.parameter_low  = (uint32_t)address,
			                  .parameter_high = (uint32_t)(address >> 32u),
			                  .control        = (XHCI_TRB_LINK << XHCI_TRB_TYPE_SHIFT) | (1u << 1u) | XHCI_TRB_CYCLE};
		if (mapping_sync(ring->dma.device_mapping_cap, 0u, ring->dma.size, DMA_SYNC_FOR_DEVICE) != SYSCALL_STATUS_OK)
			goto fail;
	}
	return true;
fail:
	xhci_ring_release(ring);
	return false;
}
void xhci_ring_release(struct xhci_ring* ring) {
	xhci_dma_buffer_release(&ring->dma);
}

bool xhci_ring_noop(struct xhci_ring* ring, uint64_t* trb_address) {
	if (ring->enqueue >= XHCI_RING_TRBS - 1u) return false; /* one command in flight */
	struct xhci_trb* trbs  = ring->dma.cpu_address;
	uint16_t         index = ring->enqueue;
	*trb_address           = (uint64_t)ring->dma.device_address + (uint64_t)index * sizeof(*trbs);
	trbs[index] = (struct xhci_trb){.control = (XHCI_TRB_NOOP_CMD << XHCI_TRB_TYPE_SHIFT) | ring->producer_cycle};
	ring->enqueue++;
	return mapping_sync(ring->dma.device_mapping_cap, 0u, ring->dma.size, DMA_SYNC_FOR_DEVICE) == SYSCALL_STATUS_OK;
}

bool xhci_ring_event(struct xhci_ring* ring, struct xhci_trb* event) {
	if (mapping_sync(ring->dma.device_mapping_cap, 0u, ring->dma.size, DMA_SYNC_FOR_CPU) != SYSCALL_STATUS_OK)
		return false;
	const struct xhci_trb* trbs    = ring->dma.cpu_address;
	const struct xhci_trb* current = &trbs[ring->dequeue];
	if ((current->control & XHCI_TRB_CYCLE) != ring->consumer_cycle) return false;
	*event = *current;
	if (++ring->dequeue == XHCI_RING_TRBS) {
		ring->dequeue = 0u;
		ring->consumer_cycle ^= 1u;
	}
	return true;
}
