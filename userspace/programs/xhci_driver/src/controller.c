#include "controller.h"

#include <base/dma.h>
#include <base/memory.h>
#include <protocol/device.h>
#include <protocol/pci.h>
#include <runtime/device.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <system/capability.h>
#include <system/memory.h>
#include <system/pci.h>
#include <system/time.h>

#include "device.h"
#include "registers.h"

#define XHCI_TIMEOUT_MS 2000u
#define XHCI_LEGACY_TIMEOUT_MS 1000u
#define XHCI_HCC_XECP_SHIFT 16u
#define XHCI_EXT_LEGACY 1u
#define XHCI_EXT_NEXT_SHIFT 8u
#define XHCI_LEGACY_BIOS_OWNED (1u << 16u)
#define XHCI_LEGACY_OS_OWNED (1u << 24u)

static bool in_bar(const struct xhci_controller* c, size_t offset, size_t width) {
	return offset <= c->size && width <= c->size - offset;
}

static uint32_t reg(const struct xhci_controller* c, size_t offset) {
	return *(volatile uint32_t*)(c->mmio + offset);
}

static void wr(const struct xhci_controller* c, size_t offset, uint32_t value) {
	*(volatile uint32_t*)(c->mmio + offset) = value;
}

static void wr64(const struct xhci_controller* c, size_t offset, uint64_t value) {
	wr(c, offset, (uint32_t)value);
	wr(c, offset + 4u, (uint32_t)(value >> 32u));
}

static bool wait_bits(const struct xhci_controller* c, size_t offset, uint32_t mask, uint32_t expected,
                      unsigned timeout_ms) {
	for (unsigned i = 0; i < timeout_ms; i++) {
		if ((reg(c, offset) & mask) == expected) return true;
		if (sleep_ms(1u) != SYSCALL_STATUS_OK) return false;
	}
	return false;
}

static bool legacy_handoff(struct xhci_controller* c) {
	uint32_t hcc    = reg(c, 0x10u);
	size_t   offset = (size_t)(hcc >> XHCI_HCC_XECP_SHIFT) * 4u;
	for (unsigned i = 0u; offset != 0u && i < 256u; i++) {
		if (!in_bar(c, offset, 8u)) return false;
		uint32_t header = reg(c, offset);
		if ((header & 0xffu) == XHCI_EXT_LEGACY) {
			wr(c, offset, header | XHCI_LEGACY_OS_OWNED);
			if (!wait_bits(c, offset, XHCI_LEGACY_BIOS_OWNED, 0u, XHCI_LEGACY_TIMEOUT_MS)) {
				fprintf(stderr, "xhci: firmware did not release controller ownership\n");
				return false;
			}
			printf("xhci: firmware ownership transferred\n");
		}
		uint32_t next = (header >> XHCI_EXT_NEXT_SHIFT) & 0xffu;
		if (next == 0u) return true;
		size_t next_offset = offset + (size_t)next * 4u;
		if (next_offset <= offset) return false;
		offset = next_offset;
	}
	return offset == 0u;
}

static bool fits_address(const struct xhci_controller* c, uint64_t address, size_t bytes) {
	if (address > UINT64_MAX - bytes) return false;
	return c->addr64 || (address + bytes - 1u <= UINT32_MAX);
}

static bool allocate(struct xhci_controller* c, struct xhci_dma_buffer* b, size_t size, size_t alignment) {
	if (!xhci_dma_buffer_allocate(&c->dma, size, MEMORY_ACCESS_READ | MEMORY_ACCESS_WRITE, b)) return false;
	if (((uintptr_t)b->device_address & (alignment - 1u)) == 0u && fits_address(c, b->device_address, size))
		return true;
	fprintf(stderr, "xhci: DMA address/alignment unsupported\n");
	return false;
}

static bool sync_device(struct xhci_dma_buffer* b) {
	return mapping_sync(b->device_mapping_cap, 0u, b->size, DMA_SYNC_FOR_DEVICE) == SYSCALL_STATUS_OK;
}

static bool enable_bus_master(struct xhci_controller* c, cap_id_t device_cap) {
	cap_id_t control_cap = CAP_ID_INVALID;
	bool     enabled     = device_resource_acquire(device_cap,
	                                               PCI_CONTROL_RESOURCE,
	                                               sizeof(PCI_CONTROL_RESOURCE) - 1u,
	                                               PCI_CONTROL_COMMAND_RIGHTS,
	                                               &control_cap) == SYSCALL_STATUS_OK &&
	                       pci_control_set_dma_enabled(control_cap, true, NULL) == SYSCALL_STATUS_OK;
	if (enabled) c->pci_control_cap = control_cap;
	else if (control_cap != CAP_ID_INVALID) (void)cap_drop(control_cap);
	if (!enabled) fprintf(stderr, "xhci: unable to enable PCI DMA\n");
	return enabled;
}

static bool allocate_scratchpads(struct xhci_controller* c) {
	if (c->scratchpad_count == 0u) return true;
	c->scratchpads = calloc(c->scratchpad_count, sizeof(*c->scratchpads));
	if (c->scratchpads == NULL ||
	    !allocate(c, &c->scratchpad_array, (size_t)c->scratchpad_count * sizeof(uint64_t), 64u))
		return false;
	uint64_t* addresses = c->scratchpad_array.cpu_address;
	for (uint16_t i = 0u; i < c->scratchpad_count; i++) {
		if (!allocate(c, &c->scratchpads[i], 4096u, 4096u)) return false;
		addresses[i] = (uint64_t)c->scratchpads[i].device_address;
	}
	if (!sync_device(&c->scratchpad_array)) return false;
	((uint64_t*)c->dcbaa.cpu_address)[0] = (uint64_t)c->scratchpad_array.device_address;
	return true;
}

bool xhci_controller_start(struct xhci_controller* c, cap_id_t device_cap) {
	size_t op = c->op;
	if (!legacy_handoff(c)) return false;
	c->owned = true;
	wr(c, op + XHCI_OP_USBCMD, reg(c, op + XHCI_OP_USBCMD) & ~XHCI_CMD_RS);
	if (!wait_bits(c, op + XHCI_OP_USBSTS, XHCI_STS_HCH, XHCI_STS_HCH, XHCI_TIMEOUT_MS)) {
		fprintf(stderr, "xhci: halt timeout\n");
		return false;
	}
	wr(c, op + XHCI_OP_USBCMD, XHCI_CMD_HCRST);
	if (!wait_bits(c, op + XHCI_OP_USBCMD, XHCI_CMD_HCRST, 0u, XHCI_TIMEOUT_MS) ||
	    !wait_bits(c, op + XHCI_OP_USBSTS, XHCI_STS_CNR, 0u, XHCI_TIMEOUT_MS)) {
		fprintf(stderr, "xhci: reset timeout\n");
		return false;
	}
	if ((reg(c, op + XHCI_OP_PAGESIZE) & 1u) == 0u) {
		fprintf(stderr, "xhci: 4 KiB controller pages unsupported\n");
		return false;
	}
	if (!xhci_dma_initialize(device_cap, &c->dma) || !enable_bus_master(c, device_cap) ||
	    !allocate(c, &c->dcbaa, 256u * sizeof(uint64_t), 64u) || !allocate_scratchpads(c) ||
	    !allocate(c, &c->erst, 16u, 64u) || !xhci_ring_allocate(&c->dma, &c->commands, true) ||
	    !xhci_ring_allocate(&c->dma, &c->events, false)) {
		fprintf(stderr, "xhci: DMA/ring allocation failed\n");
		return false;
	}
	if (!fits_address(c, c->commands.dma.device_address, c->commands.dma.size) ||
	    !fits_address(c, c->events.dma.device_address, c->events.dma.size))
		return false;
	uint64_t* erst = c->erst.cpu_address;
	/* ERST entry: segment address (64), size in TRBs (16), reserved. */
	erst[0]              = (uint64_t)c->events.dma.device_address;
	((uint32_t*)erst)[2] = XHCI_RING_TRBS;
	if (!sync_device(&c->erst) || !sync_device(&c->dcbaa)) return false;
	size_t ir = (size_t)c->runtime + XHCI_RT_IR0;
	wr(c, op + XHCI_OP_CONFIG, c->slots);
	wr64(c, op + XHCI_OP_DCBAAP, (uint64_t)c->dcbaa.device_address);
	wr64(c, op + XHCI_OP_CRCR, (uint64_t)c->commands.dma.device_address | 1u);
	wr(c, ir + XHCI_IR_IMAN, 0u); /* polling: interrupt enable stays clear */
	wr(c, ir + XHCI_IR_IMOD, 0u);
	wr(c, ir + XHCI_IR_ERSTSZ, 1u);
	wr64(c, ir + XHCI_IR_ERSTBA, (uint64_t)c->erst.device_address);
	wr64(c, ir + XHCI_IR_ERDP, (uint64_t)c->events.dma.device_address);
	wr(c, op + XHCI_OP_USBCMD, XHCI_CMD_RS);
	if (!wait_bits(c, op + XHCI_OP_USBSTS, XHCI_STS_HCH, 0u, XHCI_TIMEOUT_MS)) {
		fprintf(stderr, "xhci: start timeout\n");
		return false;
	}
	uint64_t command_address;
	if (!xhci_ring_noop(&c->commands, &command_address)) return false;
	wr(c, c->doorbell, 0u);
	for (unsigned i = 0u; i < XHCI_TIMEOUT_MS; i++) {
		uint32_t status = reg(c, op + XHCI_OP_USBSTS);
		if ((status & XHCI_STS_HSE) != 0u || (status & XHCI_STS_HCH) != 0u) {
			fprintf(stderr,
			        "xhci: controller error during No Op (sts=%08x%s%s)\n",
			        status,
			        (status & XHCI_STS_HSE) != 0u ? ", host system error" : "",
			        (status & XHCI_STS_HCH) != 0u ? ", halted" : "");
			return false;
		}
		struct xhci_trb event;
		if (xhci_ring_event(&c->events, &event)) {
			uint64_t pointer = (uint64_t)event.parameter_low | ((uint64_t)event.parameter_high << 32u);
			uint32_t type    = (event.control >> XHCI_TRB_TYPE_SHIFT) & 0x3fu;
			uint32_t code    = event.status >> XHCI_TRB_COMPLETION_SHIFT;
			wr64(c,
			     ir + XHCI_IR_ERDP,
			     (uint64_t)c->events.dma.device_address + (uint64_t)c->events.dequeue * sizeof(struct xhci_trb) | 8u);
			if (type == XHCI_TRB_COMMAND_COMPLETION && pointer == command_address && code == XHCI_TRB_SUCCESS) {
				printf("xhci: No Op command completed successfully\n");
				return true;
			}
			if (type == XHCI_TRB_COMMAND_COMPLETION) {
				fprintf(
					stderr, "xhci: No Op command failed: code=%u pointer=%llx\n", code, (unsigned long long)pointer);
				return false;
			}
			continue; /* Port-status changes may arrive before the command completion. */
		}
		if (sleep_ms(1u) != SYSCALL_STATUS_OK) return false;
	}
	fprintf(stderr, "xhci: No Op completion timed out (sts=%08x)\n", reg(c, op + XHCI_OP_USBSTS));
	return false;
}

bool xhci_controller_next_event(struct xhci_controller* c, struct xhci_trb* event) {
	if (!xhci_ring_event(&c->events, event)) return false;
	wr64(c,
	     (size_t)c->runtime + XHCI_RT_IR0 + XHCI_IR_ERDP,
	     ((uint64_t)c->events.dma.device_address + (uint64_t)c->events.dequeue * sizeof(*event)) | 8u);
	return true;
}

bool xhci_controller_queue_event(struct xhci_controller* c, struct xhci_trb event) {
	if (c->pending_count == XHCI_PENDING_EVENTS) {
		fprintf(stderr, "xhci: pending completion queue exhausted\n");
		return false;
	}
	c->pending[c->pending_count++] = event;
	return true;
}

bool xhci_controller_poll(struct xhci_controller* c) {
	struct xhci_trb event;
	for (unsigned i = 0u; i < XHCI_RING_TRBS && xhci_controller_next_event(c, &event); i++) {
		uint32_t type = (event.control >> XHCI_TRB_TYPE_SHIFT) & 0x3fu;
		if (type == XHCI_TRB_COMMAND_COMPLETION || type == 32u) {
			if (!xhci_controller_queue_event(c, event)) return false;
		}
		else if (type != 34u) {
			printf("xhci: event type=%u completion=%u\n", type, event.status >> XHCI_TRB_COMPLETION_SHIFT);
		}
	}
	if ((reg(c, c->op + XHCI_OP_USBSTS) & (1u << 2u)) != 0u) {
		fprintf(stderr, "xhci: host system error\n");
		return false;
	}
	return true;
}

void xhci_controller_stop(struct xhci_controller* c) {
	if (c->owned) xhci_device_release_all(c);
	if (c->owned && c->mmio != NULL && in_bar(c, (size_t)c->op + XHCI_OP_USBCMD, 4u)) {
		wr(c, (size_t)c->op + XHCI_OP_USBCMD, reg(c, (size_t)c->op + XHCI_OP_USBCMD) & ~XHCI_CMD_RS);
		(void)wait_bits(c, (size_t)c->op + XHCI_OP_USBSTS, XHCI_STS_HCH, XHCI_STS_HCH, XHCI_TIMEOUT_MS);
	}
	if (c->pci_control_cap != CAP_ID_INVALID) {
		(void)pci_control_set_dma_enabled(c->pci_control_cap, false, NULL);
		(void)cap_drop(c->pci_control_cap);
		c->pci_control_cap = CAP_ID_INVALID;
	}
	xhci_ring_release(&c->events);
	xhci_ring_release(&c->commands);
	xhci_dma_buffer_release(&c->erst);
	if (c->scratchpads != NULL) {
		for (uint16_t i = 0u; i < c->scratchpad_count; i++) xhci_dma_buffer_release(&c->scratchpads[i]);
		free(c->scratchpads);
		c->scratchpads = NULL;
	}
	xhci_dma_buffer_release(&c->scratchpad_array);
	xhci_dma_buffer_release(&c->dcbaa);
	xhci_dma_deinitialize(&c->dma);
}
