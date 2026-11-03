#include "transport.h"

#include <base/process.h>
#include <protocol/usb.h>
#include <runtime/device.h>
#include <stdio.h>
#include <string.h>
#include <system/capability.h>
#include <system/channel.h>
#include <system/process.h>

#include "device.h"

static uint8_t request_buffer[CAP_MAX_REQUEST_SIZE];

static bool reply(const struct cap_request* call, const void* data, size_t size, syscall_status_t status) {
	return channel_reply(call->call_id, data, size, status) == SYSCALL_STATUS_OK;
}

static bool publish_child(struct xhci_transport* t, unsigned port) {
	struct device_builder builder = {.capability = CAP_ID_INVALID, .manager_pid = PROCESS_PID_INVALID};
	char                  name[48];
	int n = snprintf(name, sizeof(name), "usb_port_%u_%llu", port, (unsigned long long)t->generation[port - 1u]);
	if (n < 0 || (size_t)n >= sizeof(name)) return false;
	syscall_status_t status = device_builder_begin_child(t->parent, &builder);
	if (status == SYSCALL_STATUS_OK) status = device_builder_set_name(&builder, name, (size_t)n);
	if (status == SYSCALL_STATUS_OK)
		status = device_builder_add_compatible(&builder, USB_COMPATIBLE_DEVICE, sizeof(USB_COMPATIBLE_DEVICE) - 1u);
	if (status == SYSCALL_STATUS_OK)
		status = device_builder_add_resource(&builder,
		                                     USB_TRANSPORT_RESOURCE,
		                                     sizeof(USB_TRANSPORT_RESOURCE) - 1u,
		                                     t->providers[port - 1u],
		                                     USB_TRANSPORT_RIGHTS);
	if (status == SYSCALL_STATUS_OK) status = device_builder_commit(&builder);
	if (status != SYSCALL_STATUS_OK) (void)device_builder_abort(&builder);
	return status == SYSCALL_STATUS_OK;
}

bool xhci_transport_init(struct xhci_transport* t, struct xhci_controller* c, cap_id_t parent) {
	struct self_info self;
	*t = (struct xhci_transport){
		.controller = c, .parent = parent, .channel = CHANNEL_ID_INVALID, .activity = CAP_ID_INVALID};
	for (unsigned i = 0u; i < 255u; i++) t->providers[i] = CAP_ID_INVALID;
	if (process_self_info(&self) != SYSCALL_STATUS_OK) return false;
	t->pid = self.pid;
	if (self.self_cap != CAP_ID_INVALID) (void)cap_drop(self.self_cap);
	if (self.address_space_cap != CAP_ID_INVALID) (void)cap_drop(self.address_space_cap);
	if (self.main_thread_cap != CAP_ID_INVALID) (void)cap_drop(self.main_thread_cap);
	if (channel_create(&t->channel, &t->activity) != SYSCALL_STATUS_OK) return false;
	for (unsigned i = 0u; i < c->ports; i++) {
		if (c->devices[i] == NULL) continue;
		t->generation[i] = c->port_generation[i];
		t->object_ids[i] = ++t->next_object_id;
		if (cap_publish(t->channel, t->object_ids[i], t->pid, USB_TRANSPORT_RIGHTS | CAP_DELEGATE, &t->providers[i]) !=
		        SYSCALL_STATUS_OK ||
		    !publish_child(t, i + 1u))
			return false;
		t->published[i] = true;
	}
	return true;
}

bool xhci_transport_poll(struct xhci_transport* t) {
	for (unsigned i = 0u; i < t->controller->ports; i++) {
		if (t->published[i] &&
		    (t->controller->devices[i] == NULL || t->generation[i] != t->controller->port_generation[i])) {
			char name[48];
			int  n = snprintf(name, sizeof(name), "usb_port_%u_%llu", i + 1u, (unsigned long long)t->generation[i]);
			if (n < 0 || (size_t)n >= sizeof(name) ||
			    device_remove_child(t->parent, name, (size_t)n) != SYSCALL_STATUS_OK ||
			    cap_unpublish(t->channel, t->object_ids[i]) != SYSCALL_STATUS_OK)
				return false;
			t->providers[i] = CAP_ID_INVALID;
			t->published[i] = false;
		}
		if (!t->published[i] && t->controller->devices[i] != NULL) {
			t->generation[i] = t->controller->port_generation[i];
			t->object_ids[i] = ++t->next_object_id;
			if (cap_publish(
					t->channel, t->object_ids[i], t->pid, USB_TRANSPORT_RIGHTS | CAP_DELEGATE, &t->providers[i]) !=
			        SYSCALL_STATUS_OK ||
			    !publish_child(t, i + 1u))
				return false;
			t->published[i] = true;
		}
	}
	for (;;) {
		struct cap_request call;
		bool               received = false;
		if (channel_recv(t->channel, &call, request_buffer, sizeof(request_buffer), &received) != SYSCALL_STATUS_OK)
			return false;
		if (!received) break;
		unsigned port = 0u;
		for (unsigned i = 0u; i < t->controller->ports; i++)
			if (t->published[i] && t->object_ids[i] == call.object_id) {
				port = i + 1u;
				break;
			}
		if (port == 0u || (call.rights & CAP_CALL) == 0u || call.request_size < sizeof(struct usb_transport_header)) {
			if (!reply(&call, NULL, 0u, SYSCALL_STATUS_DENIED)) return false;
			continue;
		}
		struct usb_transport_header header;
		memcpy(&header, request_buffer, sizeof(header));
		if (header.op == USB_TRANSPORT_OP_INFO) {
			struct usb_transport_info info;
			if (call.request_size != sizeof(header) || (call.rights & CAP_READ) == 0u ||
			    call.response_capacity < sizeof(info)) {
				if (!reply(&call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT)) return false;
			}
			else if (!xhci_device_info(t->controller, port, &info)) {
				if (!reply(&call, NULL, 0u, SYSCALL_STATUS_UNAVAILABLE)) return false;
			}
			else if (!reply(&call, &info, sizeof(info), SYSCALL_STATUS_OK)) return false;
			continue;
		}
		if (header.op == USB_TRANSPORT_OP_TRANSFER) {
			struct usb_transport_transfer_request request;
			if (call.request_size < sizeof(request) || (call.rights & CAP_WRITE) == 0u) {
				if (!reply(&call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT)) return false;
				continue;
			}
			memcpy(&request, request_buffer, sizeof(request));
			bool in = (request.endpoint & 0x80u) != 0u;
			if (request.reserved[0] || request.reserved[1] || request.reserved[2] ||
			    request.length > USB_TRANSPORT_MAX_TRANSFER ||
			    call.request_size != sizeof(request) + (in ? 0u : request.length) ||
			    call.response_capacity < sizeof(struct usb_transport_transfer_response) + (in ? request.length : 0u)) {
				if (!reply(&call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT)) return false;
				continue;
			}
			uint8_t data[sizeof(struct usb_transport_transfer_response) + USB_TRANSPORT_MAX_TRANSFER];
			if (!in && request.length != 0u)
				memcpy(data + sizeof(struct usb_transport_transfer_response),
				       request_buffer + sizeof(request),
				       request.length);
			size_t transferred = 0u;
			if (!xhci_device_transfer(t->controller,
			                          port,
			                          request.endpoint,
			                          data + sizeof(struct usb_transport_transfer_response),
			                          request.length,
			                          &transferred)) {
				if (!reply(&call, NULL, 0u, SYSCALL_STATUS_FAILED)) return false;
				continue;
			}
			struct usb_transport_transfer_response response = {.transferred = (uint32_t)transferred};
			memcpy(data, &response, sizeof(response));
			if (!reply(&call, data, sizeof(response) + (in ? transferred : 0u), SYSCALL_STATUS_OK)) return false;
			continue;
		}
		if (!reply(&call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT)) return false;
	}
	return true;
}

void xhci_transport_release(struct xhci_transport* t) {
	if (t->channel != CHANNEL_ID_INVALID) {
		for (unsigned i = 0u; i < t->controller->ports; i++)
			if (t->providers[i] != CAP_ID_INVALID) (void)cap_unpublish(t->channel, t->object_ids[i]);
		(void)channel_destroy(t->channel);
	}
	if (t->activity != CAP_ID_INVALID) (void)cap_drop(t->activity);
}
