#pragma once

#include <stdbool.h>

#include "controller.h"

/* Reconcile root-port presence with retained slots. */
bool xhci_device_poll(struct xhci_controller* controller);

/* Release every slot before stopping the controller or destroying DMA mappings. */
void xhci_device_release_all(struct xhci_controller* controller);
