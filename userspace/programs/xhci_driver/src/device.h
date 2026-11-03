#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "controller.h"

/* Reconcile root-port presence with retained slots. */
bool xhci_device_poll(struct xhci_controller* controller);

/* Release every slot before stopping the controller or destroying DMA mappings. */
void xhci_device_release_all(struct xhci_controller* controller);

/* Synchronous transfer on a configured bulk/interrupt endpoint (one outstanding request per endpoint). */
bool xhci_device_transfer(struct xhci_controller* controller, unsigned port, uint8_t endpoint_address, void* data,
                          size_t length, size_t* transferred);
