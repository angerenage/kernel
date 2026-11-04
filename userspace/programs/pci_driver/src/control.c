#include "control.h"

#include <base/process.h>
#include <protocol/pci.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <system/capability.h>
#include <system/channel.h>
#include <system/process.h>
#include <system/signal.h>

#define PCI_COMMAND_OFFSET 0x04u
#define PCI_COMMAND_IO_ENABLE (1u << 0u)
#define PCI_COMMAND_MEMORY_ENABLE (1u << 1u)
#define PCI_COMMAND_BUS_MASTER (1u << 2u)

static uint64_t object_id(const struct pci_function* function) {
	return (uint64_t)function->resources.requester_id + 1u;
}

static struct pci_function* find_function(const struct pci_control* control, uint64_t id) {
	for (struct pci_function* function = control->inventory->functions; function != NULL; function = function->next)
		if (object_id(function) == id) return function;
	return NULL;
}

static bool reply(const struct cap_request* call, const void* response, size_t response_size, syscall_status_t status) {
	if (response_size > call->response_capacity) {
		response      = NULL;
		response_size = 0u;
		status        = SYSCALL_STATUS_BAD_ARGUMENT;
	}
	return channel_reply(call->call_id, response, response_size, status) == SYSCALL_STATUS_OK;
}

static uint16_t decoder_mask(const struct pci_function* function) {
	uint16_t mask = 0u;
	for (uint8_t index = 0u; index < function->resources.bar_count; index++) {
		if (!function->resources.bars[index].claimed) continue;
		switch (function->resources.bars[index].kind) {
		case PCI_BAR_IO:
			mask |= PCI_COMMAND_IO_ENABLE;
			break;
		case PCI_BAR_MEMORY32:
		case PCI_BAR_MEMORY64:
			mask |= PCI_COMMAND_MEMORY_ENABLE;
			break;
		default:
			break;
		}
	}
	return mask;
}

static bool set_dma_enabled(struct pci_control* control, struct pci_function* function, const struct cap_request* call,
                            const void* data) {
	struct pci_control_set_dma_enabled_request  request;
	struct pci_control_set_dma_enabled_response response;
	uint16_t                                    command;
	uint16_t                                    required;
	if ((call->rights & CAP_WRITE) == 0u) return reply(call, NULL, 0u, SYSCALL_STATUS_DENIED);
	if (call->request_size != sizeof(request)) return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&request, data, sizeof(request));
	for (size_t i = 0u; i < sizeof(request.reserved); i++)
		if (request.reserved[i] != 0u) return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	if (request.enabled > 1u) return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	if (!pci_config_read16(control->config, function->address, PCI_COMMAND_OFFSET, &command))
		return reply(call, NULL, 0u, SYSCALL_STATUS_FAILED);
	required = decoder_mask(function) | PCI_COMMAND_BUS_MASTER;
	if (request.enabled != 0u) command |= required;
	else command &= (uint16_t)~PCI_COMMAND_BUS_MASTER;
	if (!pci_config_write16(control->config, function->address, PCI_COMMAND_OFFSET, command) ||
	    !pci_config_read16(control->config, function->address, PCI_COMMAND_OFFSET, &response.command) ||
	    (request.enabled != 0u && (response.command & required) != required) ||
	    (request.enabled == 0u && (response.command & PCI_COMMAND_BUS_MASTER) != 0u))
		return reply(call, NULL, 0u, SYSCALL_STATUS_FAILED);
	function->resources.command = response.command;
	return reply(call, &response, sizeof(response), SYSCALL_STATUS_OK);
}

static bool dispatch(struct pci_control* control, const struct cap_request* call, const void* data) {
	struct pci_control_request_header header;
	struct pci_function*              function = find_function(control, call->object_id);
	if (function == NULL || call->request_size < sizeof(header))
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	memcpy(&header, data, sizeof(header));
	switch (header.operation) {
	case PCI_CONTROL_OP_SET_DMA_ENABLED:
		return set_dma_enabled(control, function, call, data);
	default:
		return reply(call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT);
	}
}

bool pci_control_initialize(struct pci_control* control, struct pci_config* config, struct pci_inventory* inventory) {
	struct self_info self;
	if (control == NULL || config == NULL || inventory == NULL || process_self_info(&self) != SYSCALL_STATUS_OK)
		return false;
	*control = (struct pci_control){
		.endpoint     = CHANNEL_ID_INVALID,
		.activity_cap = CAP_ID_INVALID,
		.pid          = self.pid,
		.config       = config,
		.inventory    = inventory,
	};
	if (self.self_cap != CAP_ID_INVALID) (void)cap_drop(self.self_cap);
	if (self.address_space_cap != CAP_ID_INVALID) (void)cap_drop(self.address_space_cap);
	if (self.main_thread_cap != CAP_ID_INVALID) (void)cap_drop(self.main_thread_cap);
	return control->pid != PROCESS_PID_INVALID &&
	       channel_create(&control->endpoint, &control->activity_cap) == SYSCALL_STATUS_OK;
}

syscall_status_t pci_control_publish(struct pci_control* control, const struct pci_function* function,
                                     cap_id_t* out_capability) {
	if (control == NULL || function == NULL || out_capability == NULL) return SYSCALL_STATUS_BAD_ARGUMENT;
	*out_capability = CAP_ID_INVALID;
	return cap_publish(
		control->endpoint, object_id(function), control->pid, PCI_CONTROL_DRIVER_RIGHTS | CAP_DELEGATE, out_capability);
}

bool pci_control_serve(struct pci_control* control) {
	uint8_t request_data[CAP_MAX_REQUEST_SIZE];
	for (;;) {
		struct cap_request call;
		bool               received;
		syscall_status_t   status;
		do {
			received = false;
			status   = channel_recv(control->endpoint, &call, request_data, sizeof(request_data), &received);
			if (status != SYSCALL_STATUS_OK) return false;
			if (received) (void)dispatch(control, &call, request_data);
		} while (received);
		do {
			struct channel_event event;
			received = false;
			status   = channel_event_recv(control->endpoint, &event, &received);
			if (status != SYSCALL_STATUS_OK) return false;
		} while (received);
		struct signal_message activity;
		status = signal_wait(control->activity_cap, &activity);
		if (status != SYSCALL_STATUS_OK && status != SYSCALL_STATUS_INTERRUPTED) return false;
	}
}

void pci_control_deinitialize(struct pci_control* control) {
	if (control == NULL) return;
	if (control->activity_cap != CAP_ID_INVALID) (void)cap_drop(control->activity_cap);
	if (control->endpoint != CHANNEL_ID_INVALID) (void)channel_destroy(control->endpoint);
	*control = (struct pci_control){.endpoint = CHANNEL_ID_INVALID, .activity_cap = CAP_ID_INVALID};
}
