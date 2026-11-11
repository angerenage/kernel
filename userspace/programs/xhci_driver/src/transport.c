#include "transport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <base/process.h>

#include <protocol/usb.h>
#include <system/capability.h>
#include <system/channel.h>
#include <system/process.h>
#include <runtime/device.h>

#include "device.h"

#define XHCI_TRANSPORT_CHILDREN (255u * USB_TRANSPORT_MAX_INTERFACES)

static uint8_t request_buffer[CAP_MAX_REQUEST_SIZE];

static bool reply(const struct cap_request* call, const void* data, size_t size, syscall_status_t status) {
	return channel_reply(call->call_id, data, size, status) == SYSCALL_STATUS_OK;
}

static struct xhci_transport_child* child_at(struct xhci_transport* t, unsigned port, unsigned index) {
	return &t->children[(port - 1u) * USB_TRANSPORT_MAX_INTERFACES + index];
}

static bool child_name(char* name, size_t capacity, const struct xhci_transport_child* child, size_t* length) {
	int n = snprintf(name,
			capacity,
			"usb_port_%u_interface_%u_%llu",
			child->port,
			child->interface_number,
			(unsigned long long)child->generation);
	if (n < 0 || (size_t)n >= capacity) return false;
	*length = (size_t)n;
	return true;
}

static syscall_status_t add_u64(const struct device_builder* builder, const char* name, uint64_t value) {
	uint8_t bytes[8];
	for (unsigned i = 0u; i < 8u; i++) bytes[i] = (uint8_t)(value >> (8u * i));
	return device_builder_add_property(builder, name, strlen(name), DEVICE_PROPERTY_UNSIGNED, 1u, bytes, sizeof(bytes));
}

static bool add_compatible(
		const struct device_builder* builder, const char* format, unsigned a, unsigned b, unsigned c) {
	char name[48];
	int n = snprintf(name, sizeof(name), format, a, b, c);
	return n > 0 && (size_t)n < sizeof(name) &&
	       device_builder_add_compatible(builder, name, (size_t)n) == SYSCALL_STATUS_OK;
}

static bool publish_child(struct xhci_transport* t, struct xhci_transport_child* child) {
	struct usb_transport_info info;
	if (!xhci_device_info(t->controller, child->port, child->interface_index, &info)) return false;
	struct device_builder builder = {.capability = CAP_ID_INVALID, .manager_pid = PROCESS_PID_INVALID};
	char name[64];
	size_t length;
	if (!child_name(name, sizeof(name), child, &length)) return false;
	syscall_status_t status = device_builder_begin_child(t->parent, &builder);
	if (status == SYSCALL_STATUS_OK) status = device_builder_set_name(&builder, name, length);
	if (status == SYSCALL_STATUS_OK && !add_compatible(&builder, "usb:%04x:%04x", info.vendor_id, info.product_id, 0u))
		status = SYSCALL_STATUS_FAILED;
	if (status == SYSCALL_STATUS_OK &&
			!add_compatible(
					&builder, "usb:class:%02x:%02x:%02x", info.class_code, info.subclass, info.programming_interface))
		status = SYSCALL_STATUS_FAILED;
	if (status == SYSCALL_STATUS_OK &&
			!add_compatible(&builder, "usb:class:%02x:%02x", info.class_code, info.subclass, 0u))
		status = SYSCALL_STATUS_FAILED;
	if (status == SYSCALL_STATUS_OK && !add_compatible(&builder, "usb:class:%02x", info.class_code, 0u, 0u))
		status = SYSCALL_STATUS_FAILED;
	if (status == SYSCALL_STATUS_OK)
		status = device_builder_add_compatible(&builder, USB_COMPATIBLE_DEVICE, sizeof(USB_COMPATIBLE_DEVICE) - 1u);
	if (status == SYSCALL_STATUS_OK) status = add_u64(&builder, "vendor_id", info.vendor_id);
	if (status == SYSCALL_STATUS_OK) status = add_u64(&builder, "product_id", info.product_id);
	if (status == SYSCALL_STATUS_OK) status = add_u64(&builder, "class_code", info.class_code);
	if (status == SYSCALL_STATUS_OK) status = add_u64(&builder, "subclass", info.subclass);
	if (status == SYSCALL_STATUS_OK) status = add_u64(&builder, "programming_interface", info.programming_interface);
	if (status == SYSCALL_STATUS_OK) status = add_u64(&builder, "interface_number", info.interface_number);
	if (status == SYSCALL_STATUS_OK) status = add_u64(&builder, "configuration", info.configuration);
	if (status == SYSCALL_STATUS_OK)
		status = device_builder_add_resource(&builder,
				USB_TRANSPORT_RESOURCE,
				sizeof(USB_TRANSPORT_RESOURCE) - 1u,
				child->provider,
				USB_TRANSPORT_RIGHTS);
	if (status == SYSCALL_STATUS_OK) status = device_builder_commit(&builder);
	if (status != SYSCALL_STATUS_OK) (void)device_builder_abort(&builder);
	return status == SYSCALL_STATUS_OK;
}

static bool reconcile(struct xhci_transport* t) {
	for (unsigned port = 1u; port <= t->controller->ports; port++) {
		struct usb_transport_interface interfaces[USB_TRANSPORT_MAX_INTERFACES];
		unsigned count = xhci_device_interfaces(t->controller, port, interfaces);
		uint64_t generation = t->controller->port_generation[port - 1u];
		for (unsigned index = 0u; index < USB_TRANSPORT_MAX_INTERFACES; index++) {
			struct xhci_transport_child* child = child_at(t, port, index);
			if (child->published && (child->generation != generation || index >= count ||
											child->interface_number != interfaces[index].number)) {
				char name[64];
				size_t length;
				if (!child_name(name, sizeof(name), child, &length) ||
						device_remove_child(t->parent, name, length) != SYSCALL_STATUS_OK ||
						cap_unpublish(t->channel, child->object_id) != SYSCALL_STATUS_OK)
					return false;
				child->published = false;
				child->provider = CAP_ID_INVALID;
			}
			if (!child->published && index < count) {
				child->port = (uint8_t)port;
				child->interface_index = (uint8_t)index;
				child->interface_number = interfaces[index].number;
				child->generation = generation;
				child->object_id = ++t->next_object_id;
				if (cap_publish(t->channel,
							child->object_id,
							t->pid,
							USB_TRANSPORT_RIGHTS | CAP_DELEGATE,
							&child->provider) != SYSCALL_STATUS_OK)
					return false;
				if (!publish_child(t, child)) {
					(void)cap_unpublish(t->channel, child->object_id);
					child->provider = CAP_ID_INVALID;
					return false;
				}
				child->published = true;
			}
		}
	}
	return true;
}

bool xhci_transport_init(struct xhci_transport* t, struct xhci_controller* c, cap_id_t parent) {
	struct self_info self;
	*t = (struct xhci_transport){
		.controller = c, .parent = parent, .channel = CHANNEL_ID_INVALID, .activity = CAP_ID_INVALID
	};
	t->children = calloc(XHCI_TRANSPORT_CHILDREN, sizeof(*t->children));
	if (t->children == NULL || process_self_info(&self) != SYSCALL_STATUS_OK) return false;
	t->pid = self.pid;
	if (self.self_cap != CAP_ID_INVALID) (void)cap_drop(self.self_cap);
	if (self.address_space_cap != CAP_ID_INVALID) (void)cap_drop(self.address_space_cap);
	if (self.main_thread_cap != CAP_ID_INVALID) (void)cap_drop(self.main_thread_cap);
	if (channel_create(&t->channel, &t->activity) != SYSCALL_STATUS_OK) return false;
	return reconcile(t);
}

bool xhci_transport_poll(struct xhci_transport* t) {
	if (!reconcile(t)) return false;
	for (;;) {
		struct cap_request call;
		bool received = false;
		if (channel_recv(t->channel, &call, request_buffer, sizeof(request_buffer), &received) != SYSCALL_STATUS_OK)
			return false;
		if (!received) break;
		struct xhci_transport_child* child = NULL;
		for (unsigned i = 0u; i < XHCI_TRANSPORT_CHILDREN; i++)
			if (t->children[i].published && t->children[i].object_id == call.object_id) {
				child = &t->children[i];
				break;
			}
		if (child == NULL || (call.rights & CAP_CALL) == 0u ||
				call.request_size < sizeof(struct usb_transport_header)) {
			if (!reply(&call, NULL, 0u, SYSCALL_STATUS_DENIED)) return false;
			continue;
		}
		unsigned port = child->port;
		struct usb_transport_header header;
		memcpy(&header, request_buffer, sizeof(header));
		if (header.op == USB_TRANSPORT_OP_INFO) {
			struct usb_transport_info info;
			if (call.request_size != sizeof(header) || (call.rights & CAP_READ) == 0u ||
					call.response_capacity < sizeof(info)) {
				if (!reply(&call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT)) return false;
			}
			else if (!xhci_device_info(t->controller, port, child->interface_index, &info)) {
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
					call.response_capacity <
							sizeof(struct usb_transport_transfer_response) + (in ? request.length : 0u)) {
				if (!reply(&call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT)) return false;
				continue;
			}
			uint8_t data[sizeof(struct usb_transport_transfer_response) + USB_TRANSPORT_MAX_TRANSFER];
			if (!in && request.length != 0u)
				memcpy(data + sizeof(struct usb_transport_transfer_response),
						request_buffer + sizeof(request),
						request.length);
			struct usb_transport_info info;
			if (!xhci_device_info(t->controller, port, child->interface_index, &info)) return false;
			bool owned = false;
			for (unsigned i = 0u; i < info.endpoint_count; i++)
				if (info.endpoints[i].address == request.endpoint) owned = true;
			if (!owned) {
				if (!reply(&call, NULL, 0u, SYSCALL_STATUS_DENIED)) return false;
				continue;
			}
			size_t transferred = 0u;
			syscall_status_t transfer_status = xhci_device_transfer(t->controller,
					port,
					request.endpoint,
					data + sizeof(struct usb_transport_transfer_response),
					request.length,
					&transferred);
			if (transfer_status != SYSCALL_STATUS_OK) {
				if (!reply(&call, NULL, 0u, transfer_status)) return false;
				continue;
			}
			struct usb_transport_transfer_response response = {.transferred = (uint32_t)transferred};
			memcpy(data, &response, sizeof(response));
			if (!reply(&call, data, sizeof(response) + (in ? transferred : 0u), SYSCALL_STATUS_OK)) return false;
			continue;
		}
		if (header.op == USB_TRANSPORT_OP_CONTROL) {
			struct usb_transport_control_request request;
			if (call.request_size < sizeof(request) || (call.rights & CAP_WRITE) == 0u) {
				if (!reply(&call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT)) return false;
				continue;
			}
			memcpy(&request, request_buffer, sizeof(request));
			bool in = (request.request_type & 0x80u) != 0u;
			if (request.length > USB_TRANSPORT_MAX_TRANSFER ||
					request.length > CAP_MAX_REQUEST_SIZE - sizeof(request) ||
					request.length > CAP_MAX_RESPONSE_SIZE - sizeof(struct usb_transport_transfer_response) ||
					request.index != child->interface_number || (request.request_type & 0x1fu) != 1u ||
					call.request_size != sizeof(request) + (in ? 0u : request.length) ||
					call.response_capacity <
							sizeof(struct usb_transport_transfer_response) + (in ? request.length : 0u)) {
				if (!reply(&call, NULL, 0u, SYSCALL_STATUS_BAD_ARGUMENT)) return false;
				continue;
			}
			uint8_t data[sizeof(struct usb_transport_transfer_response) + USB_TRANSPORT_MAX_TRANSFER];
			if (!in && request.length != 0u)
				memcpy(data + sizeof(struct usb_transport_transfer_response),
						request_buffer + sizeof(request),
						request.length);
			size_t transferred = 0u;
			if (!xhci_device_control(t->controller,
						port,
						child->interface_number,
						&request,
						data + sizeof(struct usb_transport_transfer_response),
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
		if (t->children != NULL)
			for (unsigned i = 0u; i < XHCI_TRANSPORT_CHILDREN; i++)
				if (t->children[i].published) (void)cap_unpublish(t->channel, t->children[i].object_id);
		(void)channel_destroy(t->channel);
	}
	if (t->activity != CAP_ID_INVALID) (void)cap_drop(t->activity);
	free(t->children);
	t->children = NULL;
}
