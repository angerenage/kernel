#pragma once

#include <base/cap.h>
#include <base/channel.h>
#include <base/process.h>
#include <stdbool.h>

#include "controller.h"

struct xhci_transport {
	struct xhci_controller* controller;
	cap_id_t                parent;
	channel_id_t            channel;
	cap_id_t                activity;
	process_id_t            pid;
	cap_id_t                providers[255];
	bool                    published[255];
	uint64_t                generation[255];
};

/* Publish one transport resource for each enumerated USB child. */
bool xhci_transport_init(struct xhci_transport* transport, struct xhci_controller* controller, cap_id_t parent);

/* Process child transport requests and detect removed children. */
bool xhci_transport_poll(struct xhci_transport* transport);

/* Release transport capabilities and its channel. */
void xhci_transport_release(struct xhci_transport* transport);
