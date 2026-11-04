#include "device.h"

#include <base/dma.h>
#include <base/memory.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <system/memory.h>
#include <system/time.h>

#include "registers.h"

#define XHCI_DEVICE_TIMEOUT_MS 2000u
#define XHCI_PORTSC_CCS (1u << 0u)
#define XHCI_PORTSC_PED (1u << 1u)
#define XHCI_PORTSC_PR (1u << 4u)
#define XHCI_PORTSC_PP (1u << 9u)
#define XHCI_PORTSC_SPEED_SHIFT 10u
#define XHCI_PORTSC_SPEED_MASK 15u
#define XHCI_PORTSC_WRC (1u << 19u)
#define XHCI_PORTSC_PRC (1u << 21u)
#define XHCI_PORTSC_CHANGE_MASK (0x7fu << 17u)
#define XHCI_TRB_ENABLE_SLOT 9u
#define XHCI_TRB_DISABLE_SLOT 10u
#define XHCI_TRB_ADDRESS_DEVICE 11u
#define XHCI_TRB_CONFIGURE_ENDPOINT 12u
#define XHCI_TRB_NORMAL 1u
#define XHCI_TRB_SHORT_PACKET 13u
#define XHCI_TRB_SETUP_STAGE 2u
#define XHCI_TRB_DATA_STAGE 3u
#define XHCI_TRB_STATUS_STAGE 4u
#define XHCI_TRB_TRANSFER_EVENT 32u
#define XHCI_TRB_PORT_STATUS 34u
#define XHCI_TRB_IOC (1u << 5u)
#define XHCI_TRB_IDT (1u << 6u)
#define XHCI_TRB_DIR_IN (1u << 16u)
#define XHCI_TRB_TRT_IN (3u << 16u)
#define XHCI_ENDPOINT_BUFFER_SIZE 4096u
#define XHCI_USB_DESCRIPTOR_DEVICE 1u
#define XHCI_DESCRIPTOR_LENGTH 18u
#define XHCI_USB_DESCRIPTOR_CONFIGURATION 2u
#define XHCI_USB_DESCRIPTOR_INTERFACE 4u
#define XHCI_USB_DESCRIPTOR_ENDPOINT 5u
#define XHCI_CONFIGURATION_HEADER_LENGTH 9u
#define XHCI_CONFIGURATION_MAX_LENGTH 4096u
#define XHCI_MAX_ENDPOINT_DESCRIPTORS 30u

struct xhci_endpoint {
	struct xhci_ring       ring;
	struct xhci_dma_buffer buffer;
	uint64_t               pending_trb;
	size_t                 pending_length;
	size_t                 completed_length;
	uint8_t                address;
	uint8_t                attributes;
	uint8_t                interval;
	uint16_t               max_packet_size;
	bool                   configured;
	bool                   transfer_pending;
	bool                   report_ready;
	uint8_t                interface_number;
};

struct xhci_device {
	struct xhci_dma_buffer         input;
	struct xhci_dma_buffer         output;
	struct xhci_dma_buffer         descriptor;
	struct xhci_ring               endpoint_zero;
	uint8_t                        slot;
	uint8_t                        configuration;
	uint8_t                        endpoint_count;
	uint16_t                       vendor_id;
	uint16_t                       product_id;
	uint8_t                        interface_count;
	struct usb_transport_interface interfaces[USB_TRANSPORT_MAX_INTERFACES];
	struct xhci_endpoint           endpoints[XHCI_MAX_ENDPOINT_DESCRIPTORS];
};

static uint32_t read_reg(const struct xhci_controller* c, size_t offset) {
	return *(volatile uint32_t*)(c->mmio + offset);
}

static void write_reg(const struct xhci_controller* c, size_t offset, uint32_t value) {
	*(volatile uint32_t*)(c->mmio + offset) = value;
}

static bool sync_dma(struct xhci_dma_buffer* buffer, enum dma_sync_target direction) {
	return mapping_sync(buffer->device_mapping_cap, 0u, buffer->size, direction) == SYSCALL_STATUS_OK;
}

static bool allocate_dma(struct xhci_controller* c, struct xhci_dma_buffer* buffer, size_t bytes, size_t alignment) {
	if (!xhci_dma_buffer_allocate(&c->dma, bytes, MEMORY_ACCESS_READ | MEMORY_ACCESS_WRITE, buffer)) return false;
	if (((uint64_t)buffer->device_address & (alignment - 1u)) != 0u ||
	    (!c->addr64 && (uint64_t)buffer->device_address + buffer->size - 1u > UINT32_MAX)) {
		fprintf(stderr, "xhci: invalid device DMA address\n");
		return false;
	}
	return true;
}

static bool wait_event(struct xhci_controller* c, uint32_t expected_type, uint64_t expected_pointer,
                       struct xhci_trb* completion, bool allow_short) {
	for (unsigned elapsed = 0u; elapsed < XHCI_DEVICE_TIMEOUT_MS; elapsed++) {
		for (unsigned index = 0u; index < c->pending_count; index++) {
			struct xhci_trb event   = c->pending[index];
			uint32_t        type    = (event.control >> XHCI_TRB_TYPE_SHIFT) & 63u;
			uint64_t        pointer = (uint64_t)event.parameter_low | ((uint64_t)event.parameter_high << 32u);
			if (type != expected_type || pointer != expected_pointer) continue;
			c->pending[index] = c->pending[--c->pending_count];
			*completion       = event;
			uint32_t code     = event.status >> XHCI_TRB_COMPLETION_SHIFT;
			if (code != XHCI_TRB_SUCCESS && !(allow_short && code == XHCI_TRB_SHORT_PACKET))
				fprintf(stderr, "xhci: completion error type=%u code=%u\n", type, code);
			return code == XHCI_TRB_SUCCESS || (allow_short && code == XHCI_TRB_SHORT_PACKET);
		}
		struct xhci_trb event;
		if (xhci_controller_next_event(c, &event)) {
			uint32_t type = (event.control >> XHCI_TRB_TYPE_SHIFT) & 63u;
			if (type == XHCI_TRB_COMMAND_COMPLETION || type == XHCI_TRB_TRANSFER_EVENT) {
				if (!xhci_controller_queue_event(c, event)) return false;
			}
			else if (type != XHCI_TRB_PORT_STATUS) {
				printf("xhci: event type=%u completion=%u\n", type, event.status >> XHCI_TRB_COMPLETION_SHIFT);
			}
			continue;
		}
		if (sleep_ms(1u) != SYSCALL_STATUS_OK) return false;
	}
	fprintf(stderr, "xhci: event timeout type=%u pointer=%llx\n", expected_type, (unsigned long long)expected_pointer);
	return false;
}

static bool command(struct xhci_controller* c, struct xhci_trb trb, struct xhci_trb* completion) {
	uint64_t address;
	if (!xhci_ring_submit(&c->commands, trb, &address)) return false;
	write_reg(c, c->doorbell, 0u);
	return wait_event(c, XHCI_TRB_COMMAND_COMPLETION, address, completion, false);
}

static bool reset_port(struct xhci_controller* c, unsigned port, uint32_t* out_speed) {
	size_t   offset = (size_t)c->op + 0x400u + (size_t)(port - 1u) * 0x10u;
	uint32_t status = read_reg(c, offset);
	if ((status & XHCI_PORTSC_CCS) == 0u) return false;
	if ((status & XHCI_PORTSC_PP) == 0u) {
		write_reg(c, offset, XHCI_PORTSC_PP);
		if (sleep_ms(20u) != SYSCALL_STATUS_OK) return false;
		status = read_reg(c, offset);
	}
	/* USB 2.x ports require a software reset; USB 3.x ports train automatically. */
	uint32_t speed = (status >> XHCI_PORTSC_SPEED_SHIFT) & XHCI_PORTSC_SPEED_MASK;
	if (speed <= 3u) write_reg(c, offset, XHCI_PORTSC_PP | XHCI_PORTSC_PR);
	for (unsigned elapsed = 0u; elapsed < XHCI_DEVICE_TIMEOUT_MS; elapsed++) {
		status = read_reg(c, offset);
		if ((status & XHCI_PORTSC_CCS) == 0u) return false;
		if ((status & XHCI_PORTSC_PED) != 0u && (status & XHCI_PORTSC_PR) == 0u) {
			*out_speed = (status >> XHCI_PORTSC_SPEED_SHIFT) & XHCI_PORTSC_SPEED_MASK;
			write_reg(c, offset, XHCI_PORTSC_PP | (status & XHCI_PORTSC_CHANGE_MASK));
			return true;
		}
		if (sleep_ms(1u) != SYSCALL_STATUS_OK) return false;
	}
	fprintf(stderr, "xhci: port %u reset timeout (PORTSC=%08x)\n", port, status);
	return false;
}

static void release_device(struct xhci_controller* c, struct xhci_device* device) {
	if (device->slot != 0u) {
		struct xhci_trb completion;
		(void)command(
			c,
			(struct xhci_trb){.control = XHCI_TRB_DISABLE_SLOT << XHCI_TRB_TYPE_SHIFT | (uint32_t)device->slot << 24u},
			&completion);
		((uint64_t*)c->dcbaa.cpu_address)[device->slot] = 0u;
		(void)sync_dma(&c->dcbaa, DMA_SYNC_FOR_DEVICE);
	}
	for (unsigned i = 0u; i < device->endpoint_count; i++) {
		xhci_ring_release(&device->endpoints[i].ring);
		xhci_dma_buffer_release(&device->endpoints[i].buffer);
	}
	xhci_ring_release(&device->endpoint_zero);
	xhci_dma_buffer_release(&device->descriptor);
	xhci_dma_buffer_release(&device->output);
	xhci_dma_buffer_release(&device->input);
}

/* Read a descriptor through the already addressed default control endpoint. */
static bool read_descriptor(struct xhci_controller* c, struct xhci_device* device, uint8_t type, uint8_t index,
                            uint8_t* result, uint16_t length) {
	uint64_t        address;
	struct xhci_trb completion;
	if (length == 0u || length > device->descriptor.size) return false;
	struct xhci_trb setup = {
		.parameter_low  = 0x00000680u | ((uint32_t)type << 24u) | ((uint32_t)index << 16u),
		.parameter_high = (uint32_t)length << 16u,
		.status         = 8u,
		.control        = (XHCI_TRB_SETUP_STAGE << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_IDT | XHCI_TRB_TRT_IN,
	};
	if (!xhci_ring_submit(&device->endpoint_zero, setup, &address)) return false;
	struct xhci_trb data = {
		.parameter_low  = (uint32_t)device->descriptor.device_address,
		.parameter_high = (uint32_t)((uint64_t)device->descriptor.device_address >> 32u),
		.status         = length,
		.control        = (XHCI_TRB_DATA_STAGE << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_DIR_IN,
	};
	if (!xhci_ring_submit(&device->endpoint_zero, data, &address)) return false;
	struct xhci_trb status = {
		.control = (XHCI_TRB_STATUS_STAGE << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_IOC,
	};
	if (!xhci_ring_submit(&device->endpoint_zero, status, &address)) return false;
	write_reg(c, (size_t)c->doorbell + (size_t)device->slot * 4u, 1u);
	if (!wait_event(c, XHCI_TRB_TRANSFER_EVENT, address, &completion, false) ||
	    !sync_dma(&device->descriptor, DMA_SYNC_FOR_CPU))
		return false;
	memcpy(result, device->descriptor.cpu_address, length);
	return result[0] >= 2u && result[1] == type;
}

/* Collect endpoint descriptors without configuring hardware endpoints yet. */
static bool discover_endpoints(struct xhci_controller* c, struct xhci_device* device) {
	uint8_t header[XHCI_CONFIGURATION_HEADER_LENGTH];
	if (!read_descriptor(c, device, XHCI_USB_DESCRIPTOR_CONFIGURATION, 0u, header, sizeof(header)) ||
	    header[0] != sizeof(header))
		return false;
	uint16_t length = (uint16_t)header[2] | ((uint16_t)header[3] << 8u);
	if (length < sizeof(header) || length > XHCI_CONFIGURATION_MAX_LENGTH) return false;
	uint8_t* descriptors = malloc(length);
	if (descriptors == NULL) return false;
	bool success = false;
	if (!read_descriptor(c, device, XHCI_USB_DESCRIPTOR_CONFIGURATION, 0u, descriptors, length)) goto done;
	device->configuration    = descriptors[5];
	bool    active_interface = false;
	uint8_t interface_number = 0u;
	for (size_t offset = 0u; offset < length;) {
		uint8_t size = descriptors[offset];
		if (size < 2u || size > length - offset) goto done;
		uint8_t type = descriptors[offset + 1u];
		if (type == XHCI_USB_DESCRIPTOR_INTERFACE && size >= 9u) {
			active_interface = descriptors[offset + 3u] == 0u;
			interface_number = descriptors[offset + 2u];
			if (active_interface) {
				if (device->interface_count == USB_TRANSPORT_MAX_INTERFACES) goto done;
				for (unsigned i = 0u; i < device->interface_count; i++)
					if (device->interfaces[i].number == interface_number) goto done;
				device->interfaces[device->interface_count++] =
					(struct usb_transport_interface){.number                = interface_number,
					                                 .class_code            = descriptors[offset + 5u],
					                                 .subclass              = descriptors[offset + 6u],
					                                 .programming_interface = descriptors[offset + 7u]};
			}
		}
		if (type == XHCI_USB_DESCRIPTOR_ENDPOINT && active_interface && size >= 7u) {
			uint8_t attributes = descriptors[offset + 3u] & 3u;
			if (attributes == 2u || attributes == 3u) {
				if (device->endpoint_count == XHCI_MAX_ENDPOINT_DESCRIPTORS) goto done;
				unsigned index                            = device->endpoint_count++;
				device->endpoints[index].address          = descriptors[offset + 2u];
				device->endpoints[index].interface_number = interface_number;
				device->endpoints[index].attributes       = attributes;
				device->endpoints[index].max_packet_size =
					(uint16_t)descriptors[offset + 4u] | ((uint16_t)descriptors[offset + 5u] << 8u);
				device->endpoints[index].interval = descriptors[offset + 6u];
			}
		}
		offset += size;
	}
	success = device->interface_count != 0u;
done:
	free(descriptors);
	return success;
}

/* Endpoint IDs are xHCI Device Context Indices (DCI), not USB addresses. */
static unsigned endpoint_dci(uint8_t address) {
	return 2u * (address & 15u) + ((address & 0x80u) != 0u);
}

static syscall_status_t submit_interrupt_in(struct xhci_controller* c, const struct xhci_device* device,
                                            struct xhci_endpoint* ep, size_t length) {
	memset(ep->buffer.cpu_address, 0, length);
	if (!sync_dma(&ep->buffer, DMA_SYNC_FOR_DEVICE)) return SYSCALL_STATUS_FAILED;
	struct xhci_trb trb = {
		.parameter_low  = (uint32_t)ep->buffer.device_address,
		.parameter_high = (uint32_t)((uint64_t)ep->buffer.device_address >> 32u),
		.status         = (uint32_t)length,
		.control        = (XHCI_TRB_NORMAL << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_IOC | (1u << 2u),
	};
	if (!xhci_ring_submit(&ep->ring, trb, &ep->pending_trb)) return SYSCALL_STATUS_FAILED;
	ep->pending_length   = length;
	ep->transfer_pending = true;
	write_reg(c, (size_t)c->doorbell + (size_t)device->slot * 4u, endpoint_dci(ep->address));
	return SYSCALL_STATUS_UNAVAILABLE;
}

syscall_status_t xhci_device_transfer(struct xhci_controller* c, unsigned port, uint8_t address, void* data,
                                      size_t length, size_t* transferred) {
	if (transferred != NULL) *transferred = 0u;
	if (c == NULL || port == 0u || port > c->ports) return SYSCALL_STATUS_BAD_ARGUMENT;
	struct xhci_device* device = c->devices[port - 1u];
	if (device == NULL) return SYSCALL_STATUS_UNAVAILABLE;
	struct xhci_endpoint* ep = NULL;
	for (unsigned i = 0u; i < device->endpoint_count; i++) {
		if (device->endpoints[i].address == address) {
			ep = &device->endpoints[i];
			break;
		}
	}
	if (ep == NULL) return SYSCALL_STATUS_DENIED;
	if (!ep->configured || length > ep->buffer.size || (length != 0u && data == NULL))
		return SYSCALL_STATUS_BAD_ARGUMENT;
	bool in = (ep->address & 0x80u) != 0u;
	if (in && ep->attributes == 3u) {
		if (ep->report_ready) {
			size_t actual = ep->completed_length < length ? ep->completed_length : length;
			if (actual != 0u) memcpy(data, ep->buffer.cpu_address, actual);
			ep->report_ready = false;
			if (transferred != NULL) *transferred = actual;
			return SYSCALL_STATUS_OK;
		}
		if (ep->transfer_pending) return SYSCALL_STATUS_UNAVAILABLE;
		return submit_interrupt_in(c, device, ep, length);
	}
	if (!in) {
		if (length != 0u) memcpy(ep->buffer.cpu_address, data, length);
		if (!sync_dma(&ep->buffer, DMA_SYNC_FOR_DEVICE)) return SYSCALL_STATUS_FAILED;
	}
	uint64_t        trb_address;
	struct xhci_trb trb = {
		.parameter_low  = (uint32_t)ep->buffer.device_address,
		.parameter_high = (uint32_t)((uint64_t)ep->buffer.device_address >> 32u),
		.status         = (uint32_t)length,
		.control        = (XHCI_TRB_NORMAL << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_IOC | (in ? (1u << 2u) : 0u),
	};
	if (!xhci_ring_submit(&ep->ring, trb, &trb_address)) return SYSCALL_STATUS_FAILED;
	write_reg(c, (size_t)c->doorbell + (size_t)device->slot * 4u, endpoint_dci(ep->address));
	struct xhci_trb completion;
	if (!wait_event(c, XHCI_TRB_TRANSFER_EVENT, trb_address, &completion, true)) {
		ep->configured = false; /* Do not reuse a ring with an unresolved transfer. */
		return SYSCALL_STATUS_FAILED;
	}
	size_t residual = completion.status & 0xffffffu;
	if (residual > length) return SYSCALL_STATUS_FAILED;
	if (transferred != NULL) *transferred = length - residual;
	if (in && !sync_dma(&ep->buffer, DMA_SYNC_FOR_CPU)) return SYSCALL_STATUS_FAILED;
	if (in && length > residual) memcpy(data, ep->buffer.cpu_address, length - residual);
	return SYSCALL_STATUS_OK;
}

bool xhci_device_control(struct xhci_controller* c, unsigned port, uint8_t interface_number,
                         const struct usb_transport_control_request* request, void* data, size_t* transferred) {
	if (c == NULL || request == NULL || port == 0u || port > c->ports) return false;
	struct xhci_device* device = c->devices[port - 1u];
	if (device == NULL || request->length > device->descriptor.size || (request->request_type & 0x1fu) != 1u ||
	    request->index != interface_number || (request->length != 0u && data == NULL))
		return false;
	bool found = false;
	for (unsigned i = 0u; i < device->interface_count; i++)
		if (device->interfaces[i].number == interface_number) found = true;
	if (!found) return false;
	bool in = (request->request_type & 0x80u) != 0u;
	if (!in && request->length != 0u) {
		memcpy(device->descriptor.cpu_address, data, request->length);
		if (!sync_dma(&device->descriptor, DMA_SYNC_FOR_DEVICE)) return false;
	}
	uint64_t        address;
	struct xhci_trb completion;
	struct xhci_trb setup = {
		.parameter_low =
			(uint32_t)request->request_type | ((uint32_t)request->request << 8u) | ((uint32_t)request->value << 16u),
		.parameter_high = (uint32_t)request->index | ((uint32_t)request->length << 16u),
		.status         = 8u,
		.control        = (XHCI_TRB_SETUP_STAGE << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_IDT |
		                  (request->length == 0u ? 0u
		                   : in                  ? XHCI_TRB_TRT_IN
		                                         : (2u << 16u)),
	};
	if (!xhci_ring_submit(&device->endpoint_zero, setup, &address)) return false;
	uint64_t data_address = 0u;
	if (request->length != 0u) {
		struct xhci_trb stage = {
			.parameter_low  = (uint32_t)device->descriptor.device_address,
			.parameter_high = (uint32_t)((uint64_t)device->descriptor.device_address >> 32u),
			.status         = request->length,
			.control        = (XHCI_TRB_DATA_STAGE << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_IOC | (in ? XHCI_TRB_DIR_IN : 0u),
		};
		if (!xhci_ring_submit(&device->endpoint_zero, stage, &data_address)) return false;
	}
	struct xhci_trb status = {
		.control = (XHCI_TRB_STATUS_STAGE << XHCI_TRB_TYPE_SHIFT) | (in ? 0u : XHCI_TRB_DIR_IN) | XHCI_TRB_IOC,
	};
	if (!xhci_ring_submit(&device->endpoint_zero, status, &address)) return false;
	write_reg(c, (size_t)c->doorbell + (size_t)device->slot * 4u, 1u);
	size_t actual = request->length;
	if (request->length != 0u) {
		if (!wait_event(c, XHCI_TRB_TRANSFER_EVENT, data_address, &completion, true)) return false;
		size_t residual = completion.status & 0xffffffu;
		if (residual > request->length) return false;
		actual -= residual;
	}
	if (!wait_event(c, XHCI_TRB_TRANSFER_EVENT, address, &completion, false)) return false;
	if (in && actual != 0u && !sync_dma(&device->descriptor, DMA_SYNC_FOR_CPU)) return false;
	if (in && actual != 0u) memcpy(data, device->descriptor.cpu_address, actual);
	if (transferred != NULL) *transferred = actual;
	return true;
}

static bool set_configuration(struct xhci_controller* c, struct xhci_device* device) {
	uint64_t        address;
	struct xhci_trb completion;
	struct xhci_trb setup = {
		.parameter_low = 0x00000900u | ((uint32_t)device->configuration << 16u),
		.status        = 8u,
		.control       = (XHCI_TRB_SETUP_STAGE << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_IDT,
	};
	if (!xhci_ring_submit(&device->endpoint_zero, setup, &address)) return false;
	struct xhci_trb status = {
		.control = (XHCI_TRB_STATUS_STAGE << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_DIR_IN | XHCI_TRB_IOC,
	};
	if (!xhci_ring_submit(&device->endpoint_zero, status, &address)) return false;
	write_reg(c, (size_t)c->doorbell + (size_t)device->slot * 4u, 1u);
	return wait_event(c, XHCI_TRB_TRANSFER_EVENT, address, &completion, false);
}

static bool configure_endpoints(struct xhci_controller* c, struct xhci_device* device, uint32_t speed) {
	if (device->endpoint_count == 0u) return set_configuration(c, device);
	if (!sync_dma(&device->output, DMA_SYNC_FOR_CPU)) return false;
	memset(device->input.cpu_address, 0, device->input.size);
	uint8_t* input  = device->input.cpu_address;
	uint8_t* output = device->output.cpu_address;
	memcpy(input + c->context_size, output, c->context_size * 2u);
	uint32_t* control = (uint32_t*)input;
	uint32_t* slot    = (uint32_t*)(input + c->context_size);
	control[1]        = 1u;
	unsigned highest  = 1u;
	for (unsigned i = 0u; i < device->endpoint_count; i++) {
		struct xhci_endpoint* ep     = &device->endpoints[i];
		unsigned              dci    = endpoint_dci(ep->address);
		unsigned              number = ep->address & 15u;
		unsigned              packet = ep->max_packet_size & 0x7ffu;
		if (number == 0u || dci > 31u || packet == 0u || (ep->address & 0x70u) != 0u) return false;
		if (!xhci_ring_allocate(&c->dma, &ep->ring, true) ||
		    !allocate_dma(c, &ep->buffer, XHCI_ENDPOINT_BUFFER_SIZE, 64u))
			return false;
		unsigned type = ep->attributes == 2u ? 2u : 3u;
		if ((ep->address & 0x80u) != 0u) type += 4u;
		unsigned interval = 0u;
		if (ep->attributes == 3u) {
			if (ep->interval == 0u) return false;
			if (speed >= 3u) interval = ep->interval - 1u;
			else {
				/* Full/low-speed bInterval is in frames; convert to microframes. */
				unsigned frames = ep->interval - 1u;
				while (frames != 0u) {
					interval++;
					frames >>= 1u;
				}
				interval += 3u;
			}
			if (interval > 15u) return false;
		}
		uint32_t* context = (uint32_t*)(input + (dci + 1u) * c->context_size);
		uint64_t  dequeue = (uint64_t)ep->ring.dma.device_address | 1u;
		context[0]        = interval << 16u;
		context[1]        = (packet << 16u) | (type << 3u) | (3u << 1u);
		context[2]        = (uint32_t)dequeue;
		context[3]        = (uint32_t)(dequeue >> 32u);
		context[4]        = ep->attributes == 2u ? 3072u : packet;
		control[1] |= 1u << dci;
		if (dci > highest) highest = dci;
	}
	slot[0] = (slot[0] & ~(31u << 27u)) | (highest << 27u);
	if (!sync_dma(&device->input, DMA_SYNC_FOR_DEVICE)) return false;
	uint64_t        pointer = (uint64_t)device->input.device_address;
	struct xhci_trb completion;
	struct xhci_trb trb = {
		.parameter_low  = (uint32_t)pointer,
		.parameter_high = (uint32_t)(pointer >> 32u),
		.control        = (XHCI_TRB_CONFIGURE_ENDPOINT << XHCI_TRB_TYPE_SHIFT) | ((uint32_t)device->slot << 24u),
	};
	if (!command(c, trb, &completion) || !set_configuration(c, device)) return false;
	for (unsigned i = 0u; i < device->endpoint_count; i++) device->endpoints[i].configured = true;
	printf("xhci: slot %u configured %u bulk/interrupt endpoints\n", device->slot, device->endpoint_count);
	return true;
}

static struct xhci_device* enumerate_port(struct xhci_controller* c, unsigned port, uint32_t speed) {
	struct xhci_device* device = calloc(1u, sizeof(*device));
	if (device == NULL) return NULL;
	*device = (struct xhci_device){
		.input         = {.memory_cap         = CAP_ID_INVALID,
		                  .cpu_mapping_cap    = CAP_ID_INVALID,
		                  .device_mapping_cap = CAP_ID_INVALID},
		.output        = {.memory_cap         = CAP_ID_INVALID,
		                  .cpu_mapping_cap    = CAP_ID_INVALID,
		                  .device_mapping_cap = CAP_ID_INVALID},
		.descriptor    = {.memory_cap         = CAP_ID_INVALID,
		                  .cpu_mapping_cap    = CAP_ID_INVALID,
		                  .device_mapping_cap = CAP_ID_INVALID},
		.endpoint_zero = {.dma = {.memory_cap         = CAP_ID_INVALID,
		                          .cpu_mapping_cap    = CAP_ID_INVALID,
		                          .device_mapping_cap = CAP_ID_INVALID}},
	};
	struct xhci_trb completion;
	bool            success = false;
	if (!command(c, (struct xhci_trb){.control = XHCI_TRB_ENABLE_SLOT << XHCI_TRB_TYPE_SHIFT}, &completion)) goto done;
	device->slot = (uint8_t)(completion.control >> 24u);
	if (device->slot == 0u || device->slot > c->slots) goto done;
	uint32_t packet_size = speed == 4u ? 512u : speed == 3u ? 64u : 8u;
	if (!allocate_dma(c, &device->input, 33u * c->context_size, 64u) ||
	    !allocate_dma(c, &device->output, 32u * c->context_size, 64u) ||
	    !allocate_dma(c, &device->descriptor, XHCI_CONFIGURATION_MAX_LENGTH, 64u) ||
	    !xhci_ring_allocate(&c->dma, &device->endpoint_zero, true))
		goto done;
	uint32_t* input  = device->input.cpu_address;
	uint32_t* slot   = (uint32_t*)((uint8_t*)device->input.cpu_address + c->context_size);
	uint32_t* ep0    = (uint32_t*)((uint8_t*)device->input.cpu_address + 2u * c->context_size);
	input[1]         = 3u; /* Add Slot Context and Endpoint 0 Context. */
	slot[0]          = (speed << 20u) | (1u << 27u);
	slot[1]          = port << 16u;
	ep0[1]           = (3u << 1u) | (4u << 3u) | (packet_size << 16u);
	uint64_t dequeue = (uint64_t)device->endpoint_zero.dma.device_address | 1u;
	ep0[2]           = (uint32_t)dequeue;
	ep0[3]           = (uint32_t)(dequeue >> 32u);
	ep0[4]           = 8u;
	((uint64_t*)c->dcbaa.cpu_address)[device->slot] = (uint64_t)device->output.device_address;
	if (!sync_dma(&c->dcbaa, DMA_SYNC_FOR_DEVICE) || !sync_dma(&device->input, DMA_SYNC_FOR_DEVICE) ||
	    !sync_dma(&device->output, DMA_SYNC_FOR_DEVICE))
		goto done;
	uint64_t        input_address = (uint64_t)device->input.device_address;
	struct xhci_trb address       = {
		.parameter_low  = (uint32_t)input_address,
		.parameter_high = (uint32_t)(input_address >> 32u),
		.control        = (XHCI_TRB_ADDRESS_DEVICE << XHCI_TRB_TYPE_SHIFT) | (uint32_t)device->slot << 24u,
	};
	if (!command(c, address, &completion)) goto done;
	uint8_t descriptor[XHCI_DESCRIPTOR_LENGTH];
	if (!read_descriptor(c, device, XHCI_USB_DESCRIPTOR_DEVICE, 0u, descriptor, sizeof(descriptor)) ||
	    !discover_endpoints(c, device) || !configure_endpoints(c, device, speed))
		goto done;
	device->vendor_id  = (uint16_t)descriptor[8] | ((uint16_t)descriptor[9] << 8u);
	device->product_id = (uint16_t)descriptor[10] | ((uint16_t)descriptor[11] << 8u);
	printf("xhci: port %u slot %u speed-id=%u VID=%04x PID=%04x usb=%x.%02x\n",
	       port,
	       device->slot,
	       speed,
	       (unsigned)((uint16_t)descriptor[8] | ((uint16_t)descriptor[9] << 8u)),
	       (unsigned)((uint16_t)descriptor[10] | ((uint16_t)descriptor[11] << 8u)),
	       descriptor[3],
	       descriptor[2]);
	success = true;
done:
	if (!success) {
		release_device(c, device);
		free(device);
		return NULL;
	}
	return device;
}

static bool poll_interrupt_transfers(struct xhci_controller* c, struct xhci_device* device) {
	for (unsigned endpoint = 0u; endpoint < device->endpoint_count; endpoint++) {
		struct xhci_endpoint* ep = &device->endpoints[endpoint];
		if (!ep->transfer_pending) continue;
		for (unsigned index = 0u; index < c->pending_count; index++) {
			struct xhci_trb event   = c->pending[index];
			uint32_t        type    = (event.control >> XHCI_TRB_TYPE_SHIFT) & 0x3fu;
			uint64_t        pointer = (uint64_t)event.parameter_low | ((uint64_t)event.parameter_high << 32u);
			if (type != XHCI_TRB_TRANSFER_EVENT || pointer != ep->pending_trb) continue;
			c->pending[index]    = c->pending[--c->pending_count];
			ep->transfer_pending = false;
			uint32_t code        = event.status >> XHCI_TRB_COMPLETION_SHIFT;
			if (code != XHCI_TRB_SUCCESS && code != XHCI_TRB_SHORT_PACKET) {
				fprintf(stderr, "xhci: interrupt transfer failed: code=%u\n", code);
				ep->configured = false;
				return false;
			}
			size_t residual = event.status & 0xffffffu;
			if (residual > ep->pending_length || !sync_dma(&ep->buffer, DMA_SYNC_FOR_CPU)) return false;
			ep->completed_length = ep->pending_length - residual;
			ep->report_ready     = true;
			break;
		}
	}
	return true;
}

bool xhci_device_poll(struct xhci_controller* c) {
	for (unsigned port = 1u; port <= c->ports; port++) {
		unsigned index     = port - 1u;
		size_t   offset    = (size_t)c->op + 0x400u + (size_t)index * 0x10u;
		bool     connected = (read_reg(c, offset) & XHCI_PORTSC_CCS) != 0u;
		if (c->devices[index] != NULL && !poll_interrupt_transfers(c, c->devices[index])) return false;
		if (!connected) {
			if (c->devices[index] != NULL) {
				printf("xhci: port %u disconnected (slot %u)\n", port, c->devices[index]->slot);
				release_device(c, c->devices[index]);
				free(c->devices[index]);
				c->devices[index] = NULL;
			}
			c->port_seen[index]        = false;
			c->port_retry_ticks[index] = 0u;
			continue;
		}
		if (c->port_seen[index]) continue;
		if (c->port_retry_ticks[index] != 0u) {
			c->port_retry_ticks[index]--;
			continue;
		}
		uint32_t speed;
		if (!reset_port(c, port, &speed)) {
			fprintf(stderr, "xhci: unable to reset port %u\n", port);
			c->port_retry_ticks[index] = 100u;
			continue;
		}
		c->devices[index] = enumerate_port(c, port, speed);
		if (c->devices[index] == NULL) {
			fprintf(stderr, "xhci: unable to enumerate port %u\n", port);
			c->port_retry_ticks[index] = 100u;
		}
		else {
			c->port_seen[index] = true;
			c->port_generation[index]++;
		}
	}
	return true;
}

void xhci_device_release_all(struct xhci_controller* c) {
	for (unsigned index = 0u; index < c->ports; index++) {
		if (c->devices[index] == NULL) continue;
		release_device(c, c->devices[index]);
		free(c->devices[index]);
		c->devices[index]          = NULL;
		c->port_seen[index]        = false;
		c->port_retry_ticks[index] = 0u;
	}
}

bool xhci_device_info(struct xhci_controller* c, unsigned port, unsigned interface_index,
                      struct usb_transport_info* info) {
	if (c == NULL || info == NULL || port == 0u || port > c->ports || c->devices[port - 1u] == NULL) return false;
	const struct xhci_device* device = c->devices[port - 1u];
	if (interface_index >= device->interface_count) return false;
	struct usb_transport_interface interface = device->interfaces[interface_index];
	*info = (struct usb_transport_info){.vendor_id             = device->vendor_id,
	                                    .product_id            = device->product_id,
	                                    .configuration         = device->configuration,
	                                    .interface_number      = interface.number,
	                                    .class_code            = interface.class_code,
	                                    .subclass              = interface.subclass,
	                                    .programming_interface = interface.programming_interface};
	for (unsigned i = 0u; i < device->endpoint_count; i++) {
		if (device->endpoints[i].interface_number != interface.number) continue;
		info->endpoints[info->endpoint_count++] = (struct usb_transport_endpoint){
			.address          = device->endpoints[i].address,
			.attributes       = device->endpoints[i].attributes,
			.interval         = device->endpoints[i].interval,
			.interface_number = interface.number,
			.max_packet_size  = device->endpoints[i].max_packet_size,
		};
	}
	return true;
}

unsigned xhci_device_interfaces(struct xhci_controller* c, unsigned port, struct usb_transport_interface* interfaces) {
	if (c == NULL || port == 0u || port > c->ports || c->devices[port - 1u] == NULL) return 0u;
	struct xhci_device* device = c->devices[port - 1u];
	if (interfaces != NULL) memcpy(interfaces, device->interfaces, device->interface_count * sizeof(*interfaces));
	return device->interface_count;
}
