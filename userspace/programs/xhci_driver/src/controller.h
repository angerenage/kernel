#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dma.h"
#include "ring.h"

struct xhci_device;

#define XHCI_PENDING_EVENTS 256u

struct xhci_controller {
	volatile uint8_t*       mmio;
	size_t                  size;
	uint32_t                op;
	uint32_t                runtime;
	uint32_t                doorbell;
	uint8_t                 slots;
	uint8_t                 ports;
	uint8_t                 context_size;
	bool                    addr64;
	bool                    owned;
	struct xhci_dma_context dma;
	struct xhci_dma_buffer  dcbaa;
	struct xhci_dma_buffer  erst;
	struct xhci_ring        commands;
	struct xhci_ring        events;
	struct xhci_trb         pending[XHCI_PENDING_EVENTS];
	uint16_t                pending_count;
	struct xhci_device*     devices[255];
	bool                    port_seen[255];
	uint16_t                port_retry_ticks[255];
	uint64_t                port_generation[255];
};

/* Probe the controller and fill in the controller structure. */
bool xhci_controller_start(struct xhci_controller* controller, cap_id_t device_cap);

/* Poll the controller for events. */
bool xhci_controller_poll(struct xhci_controller* controller);

/* Stop the controller and release resources. */
void xhci_controller_stop(struct xhci_controller* controller);

/* Consume an event from hardware and acknowledge the event ring. */
bool xhci_controller_next_event(struct xhci_controller* controller, struct xhci_trb* event);

/* Preserve a completion until its synchronous request can retrieve it. */
bool xhci_controller_queue_event(struct xhci_controller* controller, struct xhci_trb event);
