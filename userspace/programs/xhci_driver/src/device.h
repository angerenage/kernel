#pragma once

#include <base/syscall.h>
#include <protocol/usb.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "controller.h"

/* Reconcile root-port presence with retained slots. */
bool xhci_device_poll(struct xhci_controller* controller);

/* Release every slot before stopping the controller or destroying DMA mappings. */
void xhci_device_release_all(struct xhci_controller* controller);

/* Transfer on a configured endpoint, idle interrupt-IN endpoints return UNAVAILABLE while armed. */
syscall_status_t xhci_device_transfer(struct xhci_controller* controller, unsigned port, uint8_t endpoint_address,
                                      void* data, size_t length, size_t* transferred);

/* Return the descriptors of a connected USB device. */
bool xhci_device_info(struct xhci_controller* controller, unsigned port, unsigned interface_index,
                      struct usb_transport_info* info);

/* Return active alternate-setting-zero interfaces on a physical device. */
unsigned xhci_device_interfaces(struct xhci_controller* controller, unsigned port,
                                struct usb_transport_interface* interfaces);

/* Execute one interface-recipient control transfer on endpoint zero. */
bool xhci_device_control(struct xhci_controller* controller, unsigned port, uint8_t interface_number,
                         const struct usb_transport_control_request* request, void* data, size_t* transferred);
