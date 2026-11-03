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
#define XHCI_TRB_SETUP_STAGE 2u
#define XHCI_TRB_DATA_STAGE 3u
#define XHCI_TRB_STATUS_STAGE 4u
#define XHCI_TRB_TRANSFER_EVENT 32u
#define XHCI_TRB_PORT_STATUS 34u
#define XHCI_TRB_IOC (1u << 5u)
#define XHCI_TRB_IDT (1u << 6u)
#define XHCI_TRB_DIR_IN (1u << 16u)
#define XHCI_TRB_TRT_IN (3u << 16u)
#define XHCI_USB_DESCRIPTOR_DEVICE 1u
#define XHCI_DESCRIPTOR_LENGTH 18u
#define XHCI_USB_DESCRIPTOR_CONFIGURATION 2u
#define XHCI_USB_DESCRIPTOR_INTERFACE 4u
#define XHCI_USB_DESCRIPTOR_ENDPOINT 5u
#define XHCI_CONFIGURATION_HEADER_LENGTH 9u
#define XHCI_CONFIGURATION_MAX_LENGTH 4096u
#define XHCI_MAX_ENDPOINT_DESCRIPTORS 30u

struct xhci_device {
	struct xhci_dma_buffer input;
	struct xhci_dma_buffer output;
	struct xhci_dma_buffer descriptor;
	struct xhci_ring       endpoint_zero;
	uint8_t                slot;
	uint8_t                configuration;
	uint8_t                endpoint_count;
	struct {
		uint8_t  address;
		uint8_t  attributes;
		uint16_t max_packet_size;
		uint8_t  interval;
	} endpoints[XHCI_MAX_ENDPOINT_DESCRIPTORS];
};

static uint32_t read_reg(const struct xhci_controller* c, size_t offset) {
	return *(volatile uint32_t*)(c->mmio + offset);
}

static void write_reg(const struct xhci_controller* c, size_t offset, uint32_t value) {
	*(volatile uint32_t*)(c->mmio + offset) = value;
}

static void write_reg64(const struct xhci_controller* c, size_t offset, uint64_t value) {
	write_reg(c, offset, (uint32_t)value);
	write_reg(c, offset + 4u, (uint32_t)(value >> 32u));
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

/* One event consumer, shared by command and transfer completion waits. */
static bool wait_event(struct xhci_controller* c, uint32_t expected_type, uint64_t expected_pointer,
                       struct xhci_trb* completion) {
	for (unsigned elapsed = 0u; elapsed < XHCI_DEVICE_TIMEOUT_MS; elapsed++) {
		struct xhci_trb event;
		if (xhci_ring_event(&c->events, &event)) {
			uint32_t type = (event.control >> XHCI_TRB_TYPE_SHIFT) & 63u;
			uint64_t ptr  = (uint64_t)event.parameter_low | ((uint64_t)event.parameter_high << 32u);
			uint32_t code = event.status >> XHCI_TRB_COMPLETION_SHIFT;
			write_reg64(
				c,
				(size_t)c->runtime + XHCI_RT_IR0 + XHCI_IR_ERDP,
				((uint64_t)c->events.dma.device_address + (uint64_t)c->events.dequeue * sizeof(struct xhci_trb)) | 8u);
			if (type == expected_type && ptr == expected_pointer) {
				*completion = event;
				if (code != XHCI_TRB_SUCCESS) fprintf(stderr, "xhci: completion error type=%u code=%u\n", type, code);
				return code == XHCI_TRB_SUCCESS;
			}
			if (type != XHCI_TRB_PORT_STATUS)
				printf("xhci: ignored event type=%u code=%u ptr=%llx\n", type, code, (unsigned long long)ptr);
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
	return wait_event(c, XHCI_TRB_COMMAND_COMPLETION, address, completion);
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
	if (!wait_event(c, XHCI_TRB_TRANSFER_EVENT, address, &completion) ||
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
	device->configuration = descriptors[5];
	bool active_interface = false;
	for (size_t offset = 0u; offset < length;) {
		uint8_t size = descriptors[offset];
		if (size < 2u || size > length - offset) goto done;
		uint8_t type = descriptors[offset + 1u];
		if (type == XHCI_USB_DESCRIPTOR_INTERFACE && size >= 9u) active_interface = descriptors[offset + 3u] == 0u;
		if (type == XHCI_USB_DESCRIPTOR_ENDPOINT && active_interface && size >= 7u) {
			uint8_t attributes = descriptors[offset + 3u] & 3u;
			if ((attributes == 2u || attributes == 3u) && device->endpoint_count < XHCI_MAX_ENDPOINT_DESCRIPTORS) {
				unsigned index                      = device->endpoint_count++;
				device->endpoints[index].address    = descriptors[offset + 2u];
				device->endpoints[index].attributes = attributes;
				device->endpoints[index].max_packet_size =
					(uint16_t)descriptors[offset + 4u] | ((uint16_t)descriptors[offset + 5u] << 8u);
				device->endpoints[index].interval = descriptors[offset + 6u];
			}
		}
		offset += size;
	}
	printf("xhci: configuration %u, bulk/interrupt endpoints=%u (discovery only)\n",
	       device->configuration,
	       device->endpoint_count);
	for (unsigned index = 0u; index < device->endpoint_count; index++) {
		printf("xhci: endpoint %02x %s max-packet=%u interval=%u\n",
		       device->endpoints[index].address,
		       device->endpoints[index].attributes == 2u ? "bulk" : "interrupt",
		       device->endpoints[index].max_packet_size,
		       device->endpoints[index].interval);
	}
	success = true;
done:
	free(descriptors);
	return success;
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
	    !discover_endpoints(c, device))
		goto done;
	printf("xhci: port %u slot %u speed-id=%u VID=%04x PID=%04x usb=%x.%02x\n",
	       port,
	       device->slot,
	       speed,
	       (unsigned)((uint16_t)descriptor[8] | ((uint16_t)descriptor[9] << 8u)),
	       (unsigned)((uint16_t)descriptor[10] | ((uint16_t)descriptor[11] << 8u)),
	       descriptor[3],
	       descriptor[2]);
	printf("xhci: configuration %u, bulk/interrupt endpoints=%u (discovery only)\n",
	       device->configuration,
	       device->endpoint_count);
	for (unsigned index = 0u; index < device->endpoint_count; index++) {
		printf("xhci: endpoint %02x %s max-packet=%u interval=%u\n",
		       device->endpoints[index].address,
		       device->endpoints[index].attributes == 2u ? "bulk" : "interrupt",
		       device->endpoints[index].max_packet_size,
		       device->endpoints[index].interval);
	}
	success = true;
done:
	if (!success) {
		release_device(c, device);
		free(device);
		return NULL;
	}
	return device;
}

bool xhci_device_poll(struct xhci_controller* c) {
	for (unsigned port = 1u; port <= c->ports; port++) {
		unsigned index     = port - 1u;
		size_t   offset    = (size_t)c->op + 0x400u + (size_t)index * 0x10u;
		bool     connected = (read_reg(c, offset) & XHCI_PORTSC_CCS) != 0u;
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
		else c->port_seen[index] = true;
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
