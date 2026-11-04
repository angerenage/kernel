#pragma once

#include <base/cap.h>
#include <base/channel.h>
#include <base/process.h>
#include <stdbool.h>

#include "controller.h"

struct xhci_transport_child {
	cap_id_t provider;
	uint64_t object_id;
	uint64_t generation;
	uint8_t  port;
	uint8_t  interface_index;
	uint8_t  interface_number;
	bool     published;
};

struct xhci_transport {
	struct xhci_controller*      controller;
	cap_id_t                     parent;
	channel_id_t                 channel;
	cap_id_t                     activity;
	process_id_t                 pid;
	struct xhci_transport_child* children;
	uint64_t                     next_object_id;
};

/* Publish one transport resource per active USB interface. */
bool xhci_transport_init(struct xhci_transport* transport, struct xhci_controller* controller, cap_id_t parent);

/* Process child requests and remove disconnected devices. */
bool xhci_transport_poll(struct xhci_transport* transport);

/* Release transport capabilities and its channel. */
void xhci_transport_release(struct xhci_transport* transport);
