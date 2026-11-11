#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "dma.h"

#define XHCI_RING_TRBS 256u

/* A Transfer Request Block (TRB) for the xHCI controller. */
struct xhci_trb {
	uint32_t parameter_low;
	uint32_t parameter_high;
	uint32_t status;
	uint32_t control;
};

/* Ring buffer of TRBs in DMA memory, with enqueue/dequeue indices and cycle bits. */
struct xhci_ring {
	struct xhci_dma_buffer dma;
	uint16_t enqueue;
	uint16_t dequeue;
	uint8_t producer_cycle;
	uint8_t consumer_cycle;
};

/* Allocate a ring buffer of TRBs in DMA memory. */
bool xhci_ring_allocate(const struct xhci_dma_context* dma, struct xhci_ring* ring, bool command);

/* Release the ring buffer and free its DMA memory. */
void xhci_ring_release(struct xhci_ring* ring);

/* Enqueue a no-op TRB and return its device address. */
bool xhci_ring_noop(struct xhci_ring* ring, uint64_t* trb_address);

/* Submit a TRB to a producer ring, including link TRB cycle transitions. */
bool xhci_ring_submit(struct xhci_ring* ring, struct xhci_trb trb, uint64_t* trb_address);

/* Dequeue an event TRB from the ring, if available. */
bool xhci_ring_event(struct xhci_ring* ring, struct xhci_trb* event);
