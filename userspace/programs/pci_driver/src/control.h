#pragma once

#include <stdbool.h>

#include <base/cap.h>
#include <base/channel.h>
#include <base/syscall.h>

#include <protocol/pci.h>

#include "config.h"
#include "enumerate.h"

struct pci_control {
	channel_id_t endpoint;
	cap_id_t activity_cap;
	process_id_t pid;
	struct pci_config* config;
	struct pci_inventory* inventory;
};

/* Create the capability endpoint serving every function in one PCI inventory. */
bool pci_control_initialize(struct pci_control* control, struct pci_config* config, struct pci_inventory* inventory);

/* Publish a temporary local capability for one function; the caller delegates and drops it. */
syscall_status_t pci_control_publish(
		struct pci_control* control, const struct pci_function* function, cap_id_t* out_capability);

/* Serve configuration-control calls until the endpoint fails. */
bool pci_control_serve(struct pci_control* control);

/* Tear down the endpoint and its activity signal. */
void pci_control_deinitialize(struct pci_control* control);
